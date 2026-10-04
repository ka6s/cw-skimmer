/**
 * @file cwcopyformat.h
 * @brief Color Morse copy: CQ green, a repeated call sign red
 *
 * A call sign is red once that same call appears at least twice in the
 * text being drawn. CQ is green wherever those two letters sit together.
 * Word spaces are not required, so CQ and K6XX still color inside a jammed
 * run such as CQPCQK6XXK6XX. Each decoder buffer is counted on its own.
 */

#ifndef CWCOPYFORMAT_H
#define CWCOPYFORMAT_H

#include <QColor>
#include <QString>
#include <QVector>

class QPlainTextEdit;

struct CwCopyMark {
    int begin; /* inclusive index into the QString */
    int end;   /* exclusive */
    QColor color;
};

QVector<CwCopyMark> cwCopyMarks(const QString &text);

/** Recolor the editor from cwCopyMarks whenever its text changes. */
void installCwCopyHighlighter(QPlainTextEdit *view);

/** Paint CQ and repeated call signs onto the text already in the editor. */
void applyCwCopyColors(QPlainTextEdit *view);

#endif /* CWCOPYFORMAT_H */
