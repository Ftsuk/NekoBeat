#pragma once

#include <QList>
#include <QString>

// The bits of a media entry the matcher needs. Kept free of UI types so the
// matching rules can be unit tested on their own.
struct RelinkMediaInfo
{
    QString path;
    QString displayName;
    qint64 fileSize = -1;
    qint64 modifiedMs = -1;
    qint64 durationMs = -1;
};

struct RelinkCandidate
{
    QString path;
    QString displayName;
    bool sameSize = false;
    bool sameModified = false;
    bool sameDuration = false;
    // 0..5: size 2, timestamp 2, duration 1. The file name always has to match.
    int score = 0;
    bool strong() const { return score >= 4; }
};

struct RelinkSuggestion
{
    QString missingPath;
    QString missingName;
    QList<RelinkCandidate> candidates;
    int bestScore = 0;
    // Several candidates share the best score: the user has to pick one.
    bool ambiguous = false;

    bool hasCandidate() const { return !candidates.isEmpty(); }
    bool confident() const { return !ambiguous && bestScore >= 4; }
    const RelinkCandidate* best() const
    {
        return candidates.isEmpty() ? nullptr : &candidates.first();
    }
};

// Suggests where a missing entry probably moved to.
//
// Matching stays conservative on purpose: the file name has to be identical,
// and size / timestamp / duration then say how much the two look like the same
// file. Nothing is applied automatically - the caller shows these and the user
// confirms. A pure name match (what XTPlayer does) is exactly how two different
// videos end up sharing a cover.
namespace MediaRelink
{
// Candidates per entry are capped so a common file name cannot produce a wall
// of choices.
constexpr int kMaxCandidates = 8;

QList<RelinkSuggestion> suggest(const QList<RelinkMediaInfo>& missing,
                                const QList<RelinkMediaInfo>& available);
}
