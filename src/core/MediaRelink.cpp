#include "core/MediaRelink.h"

#include <algorithm>

QList<RelinkSuggestion> MediaRelink::suggest(const QList<RelinkMediaInfo>& missing,
                                             const QList<RelinkMediaInfo>& available)
{
    QList<RelinkSuggestion> result;
    result.reserve(missing.size());

    for (const RelinkMediaInfo& lost : missing)
    {
        RelinkSuggestion suggestion;
        suggestion.missingPath = lost.path;
        suggestion.missingName = lost.displayName;

        if (!lost.displayName.isEmpty())
        {
            for (const RelinkMediaInfo& candidate : available)
            {
                if (candidate.displayName.compare(lost.displayName,
                                                  Qt::CaseInsensitive) != 0)
                {
                    continue;
                }

                RelinkCandidate entry;
                entry.path = candidate.path;
                entry.displayName = candidate.displayName;
                entry.sameSize =
                    lost.fileSize > 0 && lost.fileSize == candidate.fileSize;
                entry.sameModified =
                    lost.modifiedMs > 0 && lost.modifiedMs == candidate.modifiedMs;
                entry.sameDuration =
                    lost.durationMs > 0 && lost.durationMs == candidate.durationMs;
                entry.score = (entry.sameSize ? 2 : 0) + (entry.sameModified ? 2 : 0)
                              + (entry.sameDuration ? 1 : 0);
                suggestion.candidates.append(entry);
            }
        }

        std::sort(suggestion.candidates.begin(), suggestion.candidates.end(),
                  [](const RelinkCandidate& left, const RelinkCandidate& right) {
                      if (left.score != right.score)
                          return left.score > right.score;
                      return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
                  });

        while (suggestion.candidates.size() > kMaxCandidates)
            suggestion.candidates.removeLast();

        if (!suggestion.candidates.isEmpty())
        {
            suggestion.bestScore = suggestion.candidates.first().score;
            int bestCount = 0;
            for (const RelinkCandidate& entry : suggestion.candidates)
            {
                if (entry.score == suggestion.bestScore)
                    ++bestCount;
            }
            suggestion.ambiguous = bestCount > 1;
        }

        result.append(suggestion);
    }

    return result;
}
