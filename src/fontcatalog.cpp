#include "fontcatalog.h"

#include <QFontDatabase>

QStringList FontCatalog::monospaceFamilies(const QString &current)
{
    // Qt 5.6: families()/isFixedPitch() are non-static members, called on an instance.
    QFontDatabase db;
    QStringList fixedPitch;
    const QStringList families = db.families();
    for (const QString &family : families) {
        if (db.isFixedPitch(family))
            fixedPitch << family;
    }
    return assembleFamilyList(fixedPitch, current);
}