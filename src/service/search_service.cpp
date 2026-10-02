#include "service/search_service.h"

#include <algorithm>
#include <ctime>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "repository/resource_repository.h"
#include "util/string_util.h"

namespace nexus::service {

namespace {

struct QueryTerm
{
    enum class Field
    {
        kAny,
        kCategory,
        kTag,
        kActor,
    };

    std::string text;
    Field field = Field::kAny;
    bool required = false;
    bool excluded = false;
};

std::string QuoteFts(const std::string& text)
{
    std::string quoted = "\"";
    for (const char character : text)
    {
        if (character == '\"')
        {
            quoted += "\"\"";
        }
        else
        {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\"');
    return quoted;
}

struct ParsedQuery
{
    std::vector<QueryTerm> terms;
    bool filterVideo = false;
    bool filterImage = false;
    int favorite = -1;
    int openedDays = -1;
    char openedComparator = '<';
    std::int64_t durationSeconds = -1;
    char durationComparator = '=';
};

bool ParseDurationValue(std::string value, std::int64_t* seconds);

struct ScoredResource
{
    const Resource* resource = nullptr;
    int matches = 0;
    int relevance = 0;
};

ParsedQuery Parse(const std::wstring& query)
{
    ParsedQuery parsed;
    std::vector<std::wstring> wideTokens;
    std::wstring current;
    bool quoted = false;
    for (wchar_t character : query)
    {
        if (character == L'"')
        {
            quoted = !quoted;
            continue;
        }
        if (iswspace(character) && !quoted)
        {
            if (!current.empty())
            {
                wideTokens.push_back(std::move(current));
                current.clear();
            }
        }
        else
        {
            current.push_back(character);
        }
    }
    if (!current.empty())
    {
        wideTokens.push_back(std::move(current));
    }
    for (const std::wstring& wideToken : wideTokens)
    {
        std::string token = AsciiLower(WideToUtf8(wideToken));
        if (token == "type:video")
        {
            parsed.filterVideo = true;
            continue;
        }
        if (token == "type:image")
        {
            parsed.filterImage = true;
            continue;
        }
        if (token == "favorite:true")
        {
            parsed.favorite = 1;
            continue;
        }
        if (token == "favorite:false")
        {
            parsed.favorite = 0;
            continue;
        }
        if (token.starts_with("opened:"))
        {
            std::string value = token.substr(7);
            if (!value.empty() && (value.front() == '<' ||
                                   value.front() == '>' ||
                                   value.front() == '='))
            {
                parsed.openedComparator = value.front();
                value.erase(value.begin());
            }
            if (value.ends_with('d'))
            {
                value.pop_back();
            }
            try
            {
                parsed.openedDays = std::stoi(value);
            }
            catch (...)
            {
                parsed.openedDays = -1;
            }
            continue;
        }
        if (token.starts_with("duration:"))
        {
            std::string value = token.substr(9);
            if (!value.empty() && (value.front() == '<' ||
                                   value.front() == '>' ||
                                   value.front() == '='))
            {
                parsed.durationComparator = value.front();
                value.erase(value.begin());
            }
            std::int64_t seconds = 0;
            if (ParseDurationValue(value, &seconds))
            {
                parsed.durationSeconds = seconds;
            }
            continue;
        }

        QueryTerm term;
        if (!token.empty() && token.front() == '+')
        {
            term.required = true;
            token.erase(token.begin());
        }
        else if (!token.empty() && token.front() == '-')
        {
            term.excluded = true;
            token.erase(token.begin());
        }
        if (!token.empty())
        {
            constexpr std::pair<std::string_view, QueryTerm::Field> fields[] = {
                {"category:", QueryTerm::Field::kCategory},
                {"tag:", QueryTerm::Field::kTag},
                {"actor:", QueryTerm::Field::kActor},
            };
            for (const auto& [prefix, field] : fields)
            {
                if (token.starts_with(prefix))
                {
                    term.field = field;
                    token.erase(0, prefix.size());
                    break;
                }
            }
        }
        if (!token.empty())
        {
            term.text = std::move(token);
            parsed.terms.push_back(std::move(term));
        }
    }
    return parsed;
}

const char* FtsColumn(QueryTerm::Field field)
{
    switch (field)
    {
    case QueryTerm::Field::kCategory:
        return "category:";
    case QueryTerm::Field::kTag:
        return "tags:";
    case QueryTerm::Field::kActor:
        return "actors:";
    case QueryTerm::Field::kAny:
        return "";
    }
    return "";
}

std::string BuildFtsQuery(const ParsedQuery& parsed,
                          const AliasResolver& resolveAlias)
{
    std::vector<std::string> expressions;
    for (const QueryTerm& term : parsed.terms)
    {
        if (term.text.empty() || term.excluded)
        {
            continue;
        }
        const std::string prefix = FtsColumn(term.field);
        expressions.push_back(prefix + QuoteFts(term.text));

        // Aliases are stored as a relation to the canonical tag. Include the
        // canonical names in the FTS candidate query; MatchRelevance below
        // still assigns the alias-specific score.
        if (resolveAlias &&
            (term.field == QueryTerm::Field::kAny ||
             term.field == QueryTerm::Field::kTag))
        {
            for (const std::string& canonical : resolveAlias(term.text))
            {
                if (canonical.empty())
                {
                    continue;
                }
                expressions.push_back(prefix + QuoteFts(AsciiLower(canonical)));
            }
        }
    }
    if (expressions.empty())
    {
        // FTS5's match-all expression keeps exclusion-only and filter-only
        // queries in SQLite rather than scanning the complete vector in CPU.
        return "*";
    }
    std::string result;
    for (size_t i = 0; i < expressions.size(); ++i)
    {
        if (i != 0)
        {
            result += " OR ";
        }
        result += expressions[i];
    }
    return result;
}

bool MatchesFilenameSubstring(const Resource& resource,
                              const ParsedQuery& parsed)
{
    for (const QueryTerm& term : parsed.terms)
    {
        if (term.excluded || term.field != QueryTerm::Field::kAny ||
            term.text.empty())
        {
            continue;
        }
        if (AsciiLower(resource.originalFilename).find(term.text) !=
                std::string::npos ||
            AsciiLower(resource.title).find(term.text) != std::string::npos ||
            AsciiLower(resource.code).find(term.text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool ParseInteger(std::string_view text, std::int64_t* value)
{
    if (text.empty() || value == nullptr)
    {
        return false;
    }
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (error != std::errc() || end != text.data() + text.size() ||
        parsed < 0)
    {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParseDurationValue(std::string value, std::int64_t* seconds)
{
    if (value.empty() || seconds == nullptr)
    {
        return false;
    }
    std::int64_t multiplier = 1;
    if (value.ends_with('s'))
    {
        value.pop_back();
    }
    else if (value.ends_with('m'))
    {
        value.pop_back();
        multiplier = 60;
    }
    else if (value.ends_with('h'))
    {
        value.pop_back();
        multiplier = 60 * 60;
    }
    std::int64_t amount = 0;
    if (!ParseInteger(value, &amount) ||
        amount > std::numeric_limits<std::int64_t>::max() / multiplier)
    {
        return false;
    }
    *seconds = amount * multiplier;
    return true;
}

int MatchRelevance(const Resource& resource, const std::string& token,
                   QueryTerm::Field field, const AliasResolver& resolveAlias)
{
    if (field == QueryTerm::Field::kCategory)
    {
        return AsciiLower(resource.categoryName) == token ? 80 : 0;
    }
    if (field == QueryTerm::Field::kActor)
    {
        for (const std::string& actor : resource.actors)
        {
            if (AsciiLower(actor) == token ||
                AsciiLower(actor).find(token) != std::string::npos)
            {
                return 50;
            }
        }
        return 0;
    }
    for (const std::string& tag : resource.tags)
    {
        if (AsciiLower(tag) == token)
        {
            return 100;
        }
    }
    if (resolveAlias)
    {
        for (const std::string& canonical : resolveAlias(token))
        {
            for (const std::string& tag : resource.tags)
            {
                if (AsciiLower(tag) == AsciiLower(canonical))
                {
                    return 90;
                }
            }
        }
    }
    if (field == QueryTerm::Field::kTag)
    {
        return 0;
    }
    if (AsciiLower(resource.categoryName) == token)
    {
        return 80;
    }
    for (const std::string& actor : resource.actors)
    {
        if (AsciiLower(actor).find(token) != std::string::npos)
        {
            return 50;
        }
    }
    if (AsciiLower(resource.title).find(token) != std::string::npos ||
        AsciiLower(resource.code).find(token) != std::string::npos)
    {
        return 30;
    }
    if (AsciiLower(resource.originalFilename).find(token) != std::string::npos)
    {
        return 20;
    }
    return 0;
}

}  // namespace

std::vector<const Resource*> SearchService::Search(
    repo::ResourceRepository& repository, const std::vector<Resource>& resources,
    const std::wstring& query, const AliasResolver& resolveAlias)
{
    const ParsedQuery parsed = Parse(query);
    if (parsed.filterVideo && parsed.filterImage)
    {
        return {};
    }
    const std::vector<ResourceId> candidateIds =
        repository.SearchFts(BuildFtsQuery(parsed, resolveAlias));
    const std::unordered_set<ResourceId> candidates(candidateIds.begin(),
                                                    candidateIds.end());
    const bool hasPositiveTerms = std::any_of(
        parsed.terms.begin(), parsed.terms.end(),
        [](const QueryTerm& term) { return !term.excluded; });
    std::vector<ScoredResource> scored;
    for (const Resource& resource : resources)
    {
        // unicode61 tokenizes a Chinese filename such as “销售经理照片” as
        // one token, so FTS cannot return a contains-search for “经理”. Keep
        // FTS as the primary candidate source and supplement it only with a
        // cheap filename/title/code substring check for unqualified terms.
        if (!candidates.contains(resource.id) &&
            !MatchesFilenameSubstring(resource, parsed))
        {
            continue;
        }
        if (parsed.filterVideo && !parsed.filterImage &&
            resource.type != ResourceType::kVideo)
        {
            continue;
        }
        if (parsed.filterImage && !parsed.filterVideo &&
            resource.type != ResourceType::kImage)
        {
            continue;
        }
        if (parsed.favorite >= 0 &&
            resource.favorite != (parsed.favorite == 1))
        {
            continue;
        }
        if (parsed.openedDays >= 0)
        {
            const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
            const std::int64_t threshold = now -
                static_cast<std::int64_t>(parsed.openedDays) * 86400;
            if (resource.lastOpenTime == 0)
            {
                continue;
            }
            const bool matches = parsed.openedComparator == '>'
                ? resource.lastOpenTime < threshold
                : parsed.openedComparator == '='
                    ? resource.lastOpenTime >= threshold &&
                      resource.lastOpenTime < threshold + 86400
                    : resource.lastOpenTime >= threshold;
            if (!matches)
            {
                continue;
            }
        }
        if (parsed.durationSeconds >= 0)
        {
            const std::int64_t duration = resource.duration;
            const std::int64_t target = parsed.durationSeconds;
            const bool matches = parsed.durationComparator == '>'
                ? duration > target
                : parsed.durationComparator == '<'
                    ? duration < target
                    : duration == target;
            if (!matches)
            {
                continue;
            }
        }

        ScoredResource entry{&resource};
        bool rejected = false;
        for (const QueryTerm& term : parsed.terms)
        {
            const int relevance =
                MatchRelevance(resource, term.text, term.field, resolveAlias);
            if ((term.required && relevance == 0) ||
                (term.excluded && relevance > 0))
            {
                rejected = true;
                break;
            }
            if (!term.excluded && relevance > 0)
            {
                ++entry.matches;
                entry.relevance += relevance;
            }
        }
        if (!rejected && (!hasPositiveTerms || entry.matches > 0))
        {
            scored.push_back(entry);
        }
    }

    std::sort(scored.begin(), scored.end(),
              [](const ScoredResource& left, const ScoredResource& right) {
                  if (left.matches != right.matches)
                  {
                      return left.matches > right.matches;
                  }
                  if (left.relevance != right.relevance)
                  {
                      return left.relevance > right.relevance;
                  }
                  if (left.resource->hotScore != right.resource->hotScore)
                  {
                      return left.resource->hotScore > right.resource->hotScore;
                  }
                  return left.resource->lastOpenTime >
                         right.resource->lastOpenTime;
              });

    std::vector<const Resource*> result;
    result.reserve(scored.size());
    for (const ScoredResource& entry : scored)
    {
        result.push_back(entry.resource);
    }
    return result;
}

}  // namespace nexus::service
