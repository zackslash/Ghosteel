#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <algorithm>

// A stored family missing from the scan (uninstalled font) is still
// appended so the picker shows it.
inline QStringList assembleFamilyList(const QStringList &fixedPitchFamilies, const QString &current)
{
    QStringList result;
    result << QStringLiteral("monospace");

    QStringList sorted = fixedPitchFamilies;
    std::stable_sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });

    for (const QString &family : sorted) {
        if (family.compare(QStringLiteral("monospace"), Qt::CaseInsensitive) == 0)
            continue;
        bool duplicate = false;
        for (const QString &existing : result) {
            if (existing.compare(family, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            result << family;
    }

    if (!current.isEmpty()) {
        bool present = false;
        for (const QString &existing : result) {
            if (existing.compare(current, Qt::CaseInsensitive) == 0) {
                present = true;
                break;
            }
        }
        if (!present)
            result << current;
    }

    return result;
}

class FontCatalog : public QObject
{
    Q_OBJECT

public:
    explicit FontCatalog(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QStringList monospaceFamilies(const QString &current);
};