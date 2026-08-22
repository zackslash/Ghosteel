#include "fontcatalog.h"

#include <QFontDatabase>

QStringList FontCatalog::monospaceFamilies(const QString &current)
{
    // Called through an instance: compiles on both Qt 5.6 (statics, reachable
    // via instance) and Qt 6 (statics removed).
    QFontDatabase db;
    QStringList fixedPitch;
    const QStringList families = db.families();
    for (const QString &family : families) {
        if (db.isFixedPitch(family))
            fixedPitch << family;
    }
    return assembleFamilyList(fixedPitch, current);
}