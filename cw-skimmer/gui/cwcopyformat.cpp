/**
 * @file cwcopyformat.cpp
 * @brief Find CQ and repeated call signs in one decoder buffer
 */

#include "cwcopyformat.h"

#include <QHash>
#include <QPalette>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextCursor>

#include <algorithm>

namespace {

const QColor kCqGreen(0x00, 0xff, 0x66);
const QColor kCallRed(0xff, 0x33, 0x33);

bool isAsciiLetter(QChar c)
{
    return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('z'));
}

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

bool isTokenChar(QChar c)
{
    return isAsciiLetter(c) || isAsciiDigit(c);
}

bool lettersOnly(const QString &key, int begin, int end)
{
    for (int i = begin; i < end; ++i) {
        const QChar c = key.at(i);
        if (c < QLatin1Char('A') || c > QLatin1Char('Z')) {
            return false;
        }
    }
    return true;
}

bool digitsOnly(const QString &key, int begin, int end)
{
    for (int i = begin; i < end; ++i) {
        const QChar c = key.at(i);
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

/* (?:[A-Z]{1,2}[0-9]{1,2}|[0-9][A-Z][0-9])[A-Z]{1,4} on one whole token. */
bool isCallSign(const QString &key)
{
    const int n = key.size();
    if (n < 3 || n > 8) {
        return false;
    }

    for (int letters = 1; letters <= 2; ++letters) {
        if (!lettersOnly(key, 0, letters)) {
            break;
        }
        for (int digits = 1; digits <= 2; ++digits) {
            const int prefix = letters + digits;
            if (prefix >= n || !digitsOnly(key, letters, prefix)) {
                continue;
            }
            const int suffix = n - prefix;
            if (suffix >= 1 && suffix <= 4 && lettersOnly(key, prefix, n)) {
                return true;
            }
        }
    }

    if (n >= 4 && n <= 7
        && key.at(0) >= QLatin1Char('0') && key.at(0) <= QLatin1Char('9')
        && key.at(1) >= QLatin1Char('A') && key.at(1) <= QLatin1Char('Z')
        && key.at(2) >= QLatin1Char('0') && key.at(2) <= QLatin1Char('9')
        && lettersOnly(key, 3, n)) {
        return true;
    }
    return false;
}

class CwCopyHighlighter : public QSyntaxHighlighter {
public:
    explicit CwCopyHighlighter(QTextDocument *doc)
        : QSyntaxHighlighter(doc)
    {
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (!document() || text.isEmpty()) {
            return;
        }
        const QString all = document()->toPlainText();
        const int base = currentBlock().position();
        const QVector<CwCopyMark> marks = cwCopyMarks(all);
        for (const CwCopyMark &mark : marks) {
            const int from = std::max(0, mark.begin - base);
            const int to = std::min(text.size(), mark.end - base);
            if (to <= from) {
                continue;
            }
            QTextCharFormat fmt;
            fmt.setForeground(mark.color);
            setFormat(from, to - from, fmt);
        }
    }
};

} /* namespace */

QVector<CwCopyMark> cwCopyMarks(const QString &text)
{
    /*
     * Word spaces are often missing, so CQ and a call are found inside a
     * run of letters and digits. "C!CQPCQK6XXK6XX" still has CQ twice and
     * K6XX twice. A repeated call is painted over a CQ it overlaps.
     */
    const int n = text.size();
    const QString upper = text.toUpper();
    QVector<QColor> color(n);

    for (int i = 0; i + 1 < n; ++i) {
        if (upper.at(i) == QLatin1Char('C') && upper.at(i + 1) == QLatin1Char('Q')
            && isAsciiLetter(text.at(i)) && isAsciiLetter(text.at(i + 1))) {
            color[i] = kCqGreen;
            color[i + 1] = kCqGreen;
        }
    }

    struct Span {
        int begin;
        int end;
        QString key;
    };
    QVector<Span> calls;
    QHash<QString, int> counts;
    int i = 0;
    while (i < n) {
        if (!isTokenChar(upper.at(i))) {
            ++i;
            continue;
        }
        const int runBegin = i;
        ++i;
        while (i < n && isTokenChar(upper.at(i))) {
            ++i;
        }
        const QString run = upper.mid(runBegin, i - runBegin);
        const int runLen = run.size();
        for (int start = 0; start < runLen; ++start) {
            const int maxLen = std::min(8, runLen - start);
            for (int len = 3; len <= maxLen; ++len) {
                const QString key = run.mid(start, len);
                if (!isCallSign(key)) {
                    continue;
                }
                calls.append(Span{runBegin + start, runBegin + start + len, key});
                counts[key] = counts.value(key) + 1;
            }
        }
    }

    for (const Span &span : calls) {
        if (counts.value(span.key) < 2) {
            continue;
        }
        for (int k = span.begin; k < span.end; ++k) {
            color[k] = kCallRed;
        }
    }

    QVector<CwCopyMark> marks;
    int k = 0;
    while (k < n) {
        if (!color[k].isValid()) {
            ++k;
            continue;
        }
        int end = k + 1;
        while (end < n && color[end] == color[k]) {
            ++end;
        }
        marks.append(CwCopyMark{k, end, color[k]});
        k = end;
    }
    return marks;
}

void installCwCopyHighlighter(QPlainTextEdit *view)
{
    if (!view || !view->document()) {
        return;
    }
    new CwCopyHighlighter(view->document());
}

void applyCwCopyColors(QPlainTextEdit *view)
{
    if (!view || !view->document()) {
        return;
    }
    /*
     * setPlainText inserts with the caret's current color. After a call sign
     * is painted red, that caret stays red and the next update would draw the
     * whole pane red. Paint every character in the pane color first, then
     * only CQ and repeated call signs change color.
     */
    const QString text = view->toPlainText();
    const QColor ordinary = view->palette().color(QPalette::Text);
    QTextCharFormat base;
    base.setForeground(ordinary);

    QTextCursor edit(view->document());
    edit.beginEditBlock();
    edit.select(QTextCursor::Document);
    edit.setCharFormat(base);
    for (const CwCopyMark &mark : cwCopyMarks(text)) {
        if (mark.begin < 0 || mark.end <= mark.begin || mark.begin >= text.size()) {
            continue;
        }
        QTextCursor span(view->document());
        span.setPosition(mark.begin);
        span.setPosition(std::min(mark.end, text.size()), QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        fmt.setForeground(mark.color);
        span.setCharFormat(fmt);
    }
    edit.endEditBlock();

    QTextCursor caret = view->textCursor();
    caret.movePosition(QTextCursor::End);
    caret.setCharFormat(base);
    view->setTextCursor(caret);
    view->ensureCursorVisible();
}
