/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://www.qt.io/licensing.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "watchhandler.h"

#include "breakhandler.h"
#include "debuggeractions.h"
#include "debuggercore.h"
#include "debuggerdialogs.h"
#include "debuggerengine.h"
#include "debuggerinternalconstants.h"
#include "debuggerprotocol.h"
#include "simplifytype.h"
#include "imageviewer.h"
#include "watchutils.h"

#include <utils/algorithm.h>
#include <utils/basetreeview.h>
#include <utils/qtcassert.h>
#include <utils/savedaction.h>

#include <QDebug>
#include <QFile>
#include <QProcess>
#include <QTabWidget>
#include <QTextEdit>

#include <cstring>
#include <ctype.h>

//#define USE_WATCH_MODEL_TEST 0
//#define USE_EXPENSIVE_CHECKS 0

#if USE_WATCH_MODEL_TEST
#include <modeltest.h>
#endif

namespace Debugger {
namespace Internal {

// Creates debug output for accesses to the model.
enum { debugModel = 0 };

#define MODEL_DEBUG(s) do { if (debugModel) qDebug() << s; } while (0)

#if USE_EXPENSIVE_CHECKS
#define CHECK(s) s
#else
#define CHECK(s)
#endif


static QHash<QByteArray, int> theWatcherNames;
static QHash<QByteArray, int> theTypeFormats;
static QHash<QByteArray, int> theIndividualFormats;
static int theUnprintableBase = -1;

const char INameProperty[] = "INameProperty";
const char KeyProperty[] = "KeyProperty";

static QByteArray stripForFormat(const QByteArray &ba)
{
    QByteArray res;
    res.reserve(ba.size());
    int inArray = 0;
    for (int i = 0; i != ba.size(); ++i) {
        const char c = ba.at(i);
        if (c == '<')
            break;
        if (c == '[')
            ++inArray;
        if (c == ']')
            --inArray;
        if (c == ' ')
            continue;
        if (c == '&') // Treat references like the referenced type.
            continue;
        if (inArray && c >= '0' && c <= '9')
            continue;
        res.append(c);
    }
    return res;
}

////////////////////////////////////////////////////////////////////
//
// WatchItem
//
////////////////////////////////////////////////////////////////////

// Used to make sure the item cache is notified of construction and
// destruction of items.

class WatchItem;
typedef QList<WatchItem *> WatchItems;

WatchItem *itemConstructor(WatchModel *model, const QByteArray &iname);
void itemDestructor(WatchModel *model, WatchItem *item);

class WatchItem : public WatchData
{
public:
    WatchItem *parent; // Not owned.
    WatchItems children; // Not owned. Handled via itemDestructor().

private:
    friend WatchItem *itemConstructor(WatchModel *model, const QByteArray &iname);
    friend void itemDestructor(WatchModel *model, WatchItem *item);

    WatchItem() { parent = 0; }
    ~WatchItem() { parent = 0; }
    WatchItem(const WatchItem &); // Not implemented.
};

///////////////////////////////////////////////////////////////////////
//
// WatchModel
//
///////////////////////////////////////////////////////////////////////

class SeparatedView : public QTabWidget
{
    Q_OBJECT

public:
    SeparatedView() : QTabWidget(debuggerCore()->mainWindow())
    {
        setTabsClosable(true);
        connect(this, SIGNAL(tabCloseRequested(int)), SLOT(closeTab(int)));
        setWindowFlags(windowFlags() | Qt::Window);
        setWindowTitle(WatchHandler::tr("Debugger - Qt Creator"));

        QVariant geometry = sessionValue("DebuggerSeparateWidgetGeometry");
        if (geometry.isValid())
            setGeometry(geometry.toRect());
    }

    ~SeparatedView()
    {
        setSessionValue("DebuggerSeparateWidgetGeometry", geometry());
    }

    void removeObject(const QByteArray &key)
    {
        if (QWidget *w = findWidget(key)) {
            removeTab(indexOf(w));
            sanitize();
        }
    }

    Q_SLOT void closeTab(int index)
    {
        if (QObject *o = widget(index)) {
            QByteArray iname = o->property(INameProperty).toByteArray();
            theIndividualFormats.remove(iname);
        }
        removeTab(index);
        sanitize();
    }

    void sanitize()
    {
        if (count() == 0)
            hide();
    }

    QWidget *findWidget(const QByteArray &needle)
    {
        for (int i = count(); --i >= 0; ) {
            QWidget *w = widget(i);
            QByteArray key = w->property(KeyProperty).toByteArray();
            if (key == needle)
                return w;
        }
        return 0;
    }

    template <class T> T *prepareObject(const QByteArray &key, const QString &title)
    {
        T *t = 0;
        if (QWidget *w = findWidget(key)) {
            t = qobject_cast<T *>(w);
            if (!t)
                removeTab(indexOf(w));
        }
        if (!t) {
            t = new T;
            t->setProperty(KeyProperty, key);
            addTab(t, title);
        }

        setCurrentWidget(t);
        show();
        raise();
        return t;
    }
};


class WatchModel : public QAbstractItemModel
{
    Q_OBJECT

private:
    explicit WatchModel(WatchHandler *handler);
    ~WatchModel();

    friend WatchItem *itemConstructor(WatchModel *model, const QByteArray &iname);
    friend void itemDestructor(WatchModel *model, WatchItem *item);

public:
    int rowCount(const QModelIndex &idx = QModelIndex()) const;
    int columnCount(const QModelIndex &idx) const;

    static QString nameForFormat(int format);
    TypeFormatList typeFormatList(const WatchData &value) const;

signals:
    void currentIndexRequested(const QModelIndex &idx);
    void itemIsExpanded(const QModelIndex &idx);
    void columnAdjustmentRequested();

private:
    QVariant data(const QModelIndex &idx, int role) const;
    bool setData(const QModelIndex &idx, const QVariant &value, int role);
    QModelIndex index(int, int, const QModelIndex &idx) const;
    QModelIndex parent(const QModelIndex &idx) const;
    bool hasChildren(const QModelIndex &idx) const;
    Qt::ItemFlags flags(const QModelIndex &idx) const;
    QVariant headerData(int section, Qt::Orientation orientation,
        int role = Qt::DisplayRole) const;
    bool canFetchMore(const QModelIndex &parent) const;
    void fetchMore(const QModelIndex &parent);

    void invalidateAll(const QModelIndex &parentIndex = QModelIndex());
    void resetValueCacheRecursively(WatchItem *item);

    WatchItem *createItem(const QByteArray &iname, const QString &name, WatchItem *parent);

    friend class WatchHandler;

    WatchItem *watchItem(const QModelIndex &) const;
    QModelIndex watchIndex(const WatchItem *needle) const;
    QModelIndex watchIndexHelper(const WatchItem *needle,
        const WatchItem *parentItem, const QModelIndex &parentIndex) const;

    void insertDataItem(const WatchData &data, bool destructive);
    Q_SLOT void reinsertAllData();
    void reinsertAllDataHelper(WatchItem *item, QList<WatchData> *data);
    bool ancestorChanged(const QSet<QByteArray> &parentINames, WatchItem *item) const;
    void insertBulkData(const QList<WatchData> &data);
    QString displayForAutoTest(const QByteArray &iname) const;
    void reinitialize(bool includeInspectData = false);
    void destroyItem(WatchItem *item); // With model notification.
    void destroyChildren(WatchItem *item); // With model notification.
    void destroyHelper(const WatchItems &items); // Without model notification.
    void emitDataChanged(int column,
        const QModelIndex &parentIndex = QModelIndex());

    friend QDebug operator<<(QDebug d, const WatchModel &m);

    void dump();
    void dumpHelper(WatchItem *item);
    Q_SLOT void emitAllChanged();

    void showInEditorHelper(QString *contents, WatchItem *item, int level);
    void setCurrentItem(const QByteArray &iname);

    QString displayType(const WatchData &typeIn) const;
    QString displayName(const WatchItem *item) const;
    QString displayValue(const WatchData &data) const;
    QString formattedValue(const WatchData &data) const;
    QString removeNamespaces(QString str) const;
    void formatRequests(QByteArray *out, const WatchItem *item) const;
    DebuggerEngine *engine() const;
    int itemFormat(const WatchData &data) const;
    bool contentIsValid() const;

    WatchHandler *m_handler; // Not owned.

    WatchItem *m_root; // Owned.
    WatchItem *m_localsRoot; // Not owned.
    WatchItem *m_inspectorRoot; // Not owned.
    WatchItem *m_watchRoot; // Not owned.
    WatchItem *m_returnRoot; // Not owned.
    WatchItem *m_tooltipRoot; // Not owned.

    QSet<QByteArray> m_expandedINames;
    QSet<QByteArray> m_fetchTriggered;

    TypeFormatList builtinTypeFormatList(const WatchData &data) const;
    QStringList dumperTypeFormatList(const WatchData &data) const;
    DumperTypeFormats m_reportedTypeFormats;

    WatchItem *createItem(const QByteArray &iname);
    WatchItem *createItem(const WatchData &data);
    void assignData(WatchItem *item, const WatchData &data);
    WatchItem *findItem(const QByteArray &iname) const;
    friend class WatchItem;
    typedef QHash<QByteArray, WatchItem *> Cache;
    Cache m_cache;
    typedef QHash<QByteArray, QString> ValueCache;
    ValueCache m_valueCache;

    #if USE_EXPENSIVE_CHECKS
    QHash<const WatchItem *, QByteArray> m_cache2;
    void checkTree();
    void checkItem(const WatchItem *item) const;
    void checkTree(WatchItem *item, QSet<QByteArray> *inames);
    #endif
    void checkIndex(const QModelIndex &index) const;
};

WatchModel::WatchModel(WatchHandler *handler)
    : m_handler(handler)
{
    setObjectName(QLatin1String("WatchModel"));
    m_root = createItem(QByteArray(), tr("Root"), 0);
    // Note: Needs to stay
    m_localsRoot = createItem("local", tr("Locals"), m_root);
    m_inspectorRoot = createItem("inspect", tr("Inspector"), m_root);
    m_watchRoot = createItem("watch", tr("Expressions"), m_root);
    m_returnRoot = createItem("return", tr("Return Value"), m_root);
    m_tooltipRoot = createItem("tooltip", tr("Tooltip"), m_root);

    connect(action(SortStructMembers), SIGNAL(valueChanged(QVariant)),
        SLOT(reinsertAllData()));
    connect(action(ShowStdNamespace), SIGNAL(valueChanged(QVariant)),
        SLOT(reinsertAllData()));
    connect(action(ShowQtNamespace), SIGNAL(valueChanged(QVariant)),
        SLOT(reinsertAllData()));
}

WatchModel::~WatchModel()
{
    CHECK(checkItem(m_root));
    destroyChildren(m_root);
    itemDestructor(this, m_root);
    QTC_CHECK(m_cache.isEmpty());
}

WatchItem *itemConstructor(WatchModel *model, const QByteArray &iname)
{
    QTC_CHECK(!model->m_cache.contains(iname));
    WatchItem *item = new WatchItem();
    item->iname = iname;
    model->m_cache[iname] = item;
    CHECK(model->m_cache2[item] = iname);
    CHECK(model->checkItem(item));
    return item;
}

void itemDestructor(WatchModel *model, WatchItem *item)
{
    QTC_ASSERT(model->m_cache.value(item->iname) == item, return);
    CHECK(model->checkItem(item));
    CHECK(model->m_cache2.remove(item));
    model->m_cache.remove(item->iname);
    delete item;
}

WatchItem *WatchModel::createItem(const QByteArray &iname, const QString &name, WatchItem *parent)
{
    WatchItem *item = itemConstructor(this, iname);
    item->name = name;
    item->hasChildren = true; // parent == 0;
    item->state = 0;
    item->parent = parent;
    if (parent)
        parent->children.append(item);
    return item;
}

void WatchModel::reinitialize(bool includeInspectData)
{
    CHECK(checkTree());
    //MODEL_DEBUG("REMOVING " << n << " CHILDREN OF " << m_root->iname);
    QTC_CHECK(m_root->children.size() == 5);
    destroyChildren(m_localsRoot);
    destroyChildren(m_watchRoot);
    destroyChildren(m_returnRoot);
    destroyChildren(m_tooltipRoot);
    if (includeInspectData) {
        destroyChildren(m_inspectorRoot);
        QTC_CHECK(m_cache.size() == 6);
    }
    CHECK(checkTree());
}

void WatchModel::emitAllChanged()
{
    emit layoutChanged();
}

DebuggerEngine *WatchModel::engine() const
{
    return m_handler->m_engine;
}

void WatchModel::dump()
{
    qDebug() << "\n";
    foreach (WatchItem *child, m_root->children)
        dumpHelper(child);
}

void WatchModel::dumpHelper(WatchItem *item)
{
    qDebug() << "ITEM: " << item->iname
        << (item->parent ? item->parent->iname : "<none>");
    foreach (WatchItem *child, item->children)
        dumpHelper(child);
}

void WatchModel::destroyHelper(const WatchItems &items)
{
    for (int i = items.size(); --i >= 0; ) {
        WatchItem *item = items.at(i);
        destroyHelper(item->children);
        itemDestructor(this, item);
    }
}

void WatchModel::destroyItem(WatchItem *item)
{
    const QByteArray iname = item->iname;
    CHECK(checkTree());
    QTC_ASSERT(m_cache.contains(iname), return);

    // Deregister from model and parent.
    // It's sufficient to do this non-recursively.
    WatchItem *parent = item->parent;
    QTC_ASSERT(parent, return);
    QModelIndex parentIndex = watchIndex(parent);
    checkIndex(parentIndex);
    const int i = parent->children.indexOf(item);
    //MODEL_DEBUG("NEED TO REMOVE: " << item->iname << "AT" << n);
    beginRemoveRows(parentIndex, i, i);
    parent->children.removeAt(i);
    endRemoveRows();

    // Destroy contents.
    destroyHelper(item->children);
    itemDestructor(this, item);
    QTC_ASSERT(!m_cache.contains(iname), return);
    CHECK(checkTree());
}

void WatchModel::destroyChildren(WatchItem *item)
{
    CHECK(checkTree());
    QTC_ASSERT(m_cache.contains(item->iname), return);
    if (item->children.isEmpty())
        return;

    WatchItems items = item->children;

    // Deregister from model and parent.
    // It's sufficient to do this non-recursively.
    QModelIndex idx = watchIndex(item);
    checkIndex(idx);
    beginRemoveRows(idx, 0, items.size() - 1);
    item->children.clear();
    endRemoveRows();

    // Destroy contents.
    destroyHelper(items);
    CHECK(checkTree());
}

WatchItem *WatchModel::findItem(const QByteArray &iname) const
{
    return m_cache.value(iname, 0);
}

void WatchModel::checkIndex(const QModelIndex &index) const
{
    if (index.isValid()) {
        QTC_CHECK(index.model() == this);
    } else {
        QTC_CHECK(index.model() == 0);
    }
}

WatchItem *WatchModel::createItem(const WatchData &data)
{
    WatchItem *item = itemConstructor(this, data.iname);
    static_cast<WatchData &>(*item) = data;
    return item;
}

void WatchModel::assignData(WatchItem *item, const WatchData &data)
{
    CHECK(checkItem(item));
    QTC_ASSERT(data.iname == item->iname,
        m_cache.remove(item->iname);
        m_cache[data.iname] = item);
    static_cast<WatchData &>(*item) = data;
    CHECK(checkItem(item));
}

void WatchModel::reinsertAllData()
{
    QList<WatchData> list;
    reinsertAllDataHelper(m_root, &list);
    reinitialize(true);
    insertBulkData(list);
}

void WatchModel::reinsertAllDataHelper(WatchItem *item, QList<WatchData> *data)
{
    data->append(*item);
    data->back().setAllUnneeded();
    foreach (WatchItem *child, item->children)
        reinsertAllDataHelper(child, data);
}

static QByteArray parentName(const QByteArray &iname)
{
    const int pos = iname.lastIndexOf('.');
    return pos == -1 ? QByteArray() : iname.left(pos);
}

static QString niceTypeHelper(const QByteArray &typeIn)
{
    typedef QMap<QByteArray, QString> Cache;
    static Cache cache;
    const Cache::const_iterator it = cache.constFind(typeIn);
    if (it != cache.constEnd())
        return it.value();
    const QString simplified = simplifyType(QLatin1String(typeIn));
    cache.insert(typeIn, simplified); // For simplicity, also cache unmodified types
    return simplified;
}

QString WatchModel::removeNamespaces(QString str) const
{
    if (!boolSetting(ShowStdNamespace))
        str.remove(QLatin1String("std::"));
    if (!boolSetting(ShowQtNamespace)) {
        const QString qtNamespace = QString::fromLatin1(engine()->qtNamespace());
        if (!qtNamespace.isEmpty())
            str.remove(qtNamespace);
    }
    return str;
}

static int formatToIntegerBase(int format)
{
    switch (format) {
        case HexadecimalIntegerFormat:
            return 16;
        case BinaryIntegerFormat:
            return 2;
        case OctalIntegerFormat:
            return 8;
    }
    return 10;
}

template <class IntType> QString reformatInteger(IntType value, int format)
{
    switch (format) {
        case HexadecimalIntegerFormat:
            return QLatin1String("(hex) ") + QString::number(value, 16);
        case BinaryIntegerFormat:
            return QLatin1String("(bin) ") + QString::number(value, 2);
        case OctalIntegerFormat:
            return QLatin1String("(oct) ") + QString::number(value, 8);
    }
    return QString::number(value, 10); // not reached
}

static QString reformatInteger(quint64 value, int format, int size, bool isSigned)
{
    // Follow convention and don't show negative non-decimal numbers.
    if (format != AutomaticFormat && format != DecimalIntegerFormat)
        isSigned = false;

    switch (size) {
        case 1:
            value = value & 0xff;
            return isSigned
                ? reformatInteger<qint8>(value, format)
                : reformatInteger<quint8>(value, format);
        case 2:
            value = value & 0xffff;
            return isSigned
                ? reformatInteger<qint16>(value, format)
                : reformatInteger<quint16>(value, format);
        case 4:
            value = value & 0xffffffff;
            return isSigned
                ? reformatInteger<qint32>(value, format)
                : reformatInteger<quint32>(value, format);
        default:
        case 8: return isSigned
                ? reformatInteger<qint64>(value, format)
                : reformatInteger<quint64>(value, format);
    }
}

// Format printable (char-type) characters
static QString reformatCharacter(int code, int format)
{
    const QString codeS = reformatInteger(code, format, 1, true);
    if (code < 0) // Append unsigned value.
        return codeS + QLatin1String(" / ") + reformatInteger(256 + code, format, 1, false);
    const QChar c = QChar(uint(code));
    if (c.isPrint())
        return codeS + QLatin1String(" '") + c + QLatin1Char('\'');
    switch (code) {
    case 0:
        return codeS + QLatin1String(" '\\0'");
    case '\r':
        return codeS + QLatin1String(" '\\r'");
    case '\t':
        return codeS + QLatin1String(" '\\t'");
    case '\n':
        return codeS + QLatin1String(" '\\n'");
    }
    return codeS;
}

static QString quoteUnprintable(const QString &str)
{
    if (WatchHandler::unprintableBase() == 0)
        return str;

    QString encoded;
    if (WatchHandler::unprintableBase() == -1) {
        foreach (const QChar c, str) {
            int u = c.unicode();
            if (c.isPrint())
                encoded += c;
            else if (u == '\r')
                encoded += QLatin1String("\\r");
            else if (u == '\t')
                encoded += QLatin1String("\\t");
            else if (u == '\n')
                encoded += QLatin1String("\\n");
            else
                encoded += QString::fromLatin1("\\%1")
                    .arg(c.unicode(), 3, 8, QLatin1Char('0'));
        }
        return encoded;
    }

    foreach (const QChar c, str) {
        if (c.isPrint()) {
            encoded += c;
        } else if (WatchHandler::unprintableBase() == 8) {
            encoded += QString::fromLatin1("\\%1")
                .arg(c.unicode(), 3, 8, QLatin1Char('0'));
        } else {
            encoded += QString::fromLatin1("\\u%1")
                .arg(c.unicode(), 4, 16, QLatin1Char('0'));
        }
    }
    return encoded;
}

static QString translate(const QString &str)
{
    if (str.startsWith(QLatin1Char('<'))) {
        if (str == QLatin1String("<empty>"))
            return WatchHandler::tr("<empty>");
        if (str == QLatin1String("<uninitialized>"))
            return WatchHandler::tr("<uninitialized>");
        if (str == QLatin1String("<invalid>"))
            return WatchHandler::tr("<invalid>");
        if (str == QLatin1String("<not accessible>"))
            return WatchHandler::tr("<not accessible>");
        if (str.endsWith(QLatin1String(" items>"))) {
            // '<10 items>' or '<>10 items>' (more than)
            bool ok;
            const bool moreThan = str.at(1) == QLatin1Char('>');
            const int numberPos = moreThan ? 2 : 1;
            const int len = str.indexOf(QLatin1Char(' ')) - numberPos;
            const int size = str.mid(numberPos, len).toInt(&ok);
            QTC_ASSERT(ok, qWarning("WatchHandler: Invalid item count '%s'",
                qPrintable(str)));
            return moreThan ?
                     WatchHandler::tr("<more than %n items>", 0, size) :
                     WatchHandler::tr("<%n items>", 0, size);
        }
    }
    return quoteUnprintable(str);
}

QString WatchModel::formattedValue(const WatchData &data) const
{
    const QString &value = data.value;

    if (data.type == "bool") {
        if (value == QLatin1String("0"))
            return QLatin1String("false");
        if (value == QLatin1String("1"))
            return QLatin1String("true");
        return value;
    }

    const int format = itemFormat(data);

    // Append quoted, printable character also for decimal.
    if (data.type.endsWith("char") || data.type.endsWith("QChar")) {
        bool ok;
        const int code = value.toInt(&ok);
        return ok ? reformatCharacter(code, format) : value;
    }

    if (format == HexadecimalIntegerFormat
            || format == DecimalIntegerFormat
            || format == OctalIntegerFormat
            || format == BinaryIntegerFormat) {
        bool isSigned = value.startsWith(QLatin1Char('-'));
        quint64 raw = isSigned ? quint64(value.toLongLong()): value.toULongLong();
        return reformatInteger(raw, format, data.size, isSigned);
    }

    if (format == ScientificFloatFormat) {
        double d = value.toDouble();
        return QString::number(d, 'e');
    }

    if (format == CompactFloatFormat) {
        double d = value.toDouble();
        return QString::number(d, 'g');
    }

    if (data.type == "va_list")
        return value;

    if (!isPointerType(data.type) && !data.isVTablePointer()) {
        bool ok = false;
        qulonglong integer = value.toULongLong(&ok, 0);
        if (ok) {
            const int format = itemFormat(data);
            return reformatInteger(integer, format, data.size, false);
        }
    }

    if (data.elided) {
        QString v = value;
        v.chop(1);
        v = translate(v);
        QString len = data.elided > 0 ? QString::number(data.elided)
                                      : QLatin1String("unknown length");
        return v + QLatin1String("\"... (") + len  + QLatin1Char(')');
    }

    return translate(value);
}

// Get a pointer address from pointer values reported by the debugger.
// Fix CDB formatting of pointers "0x00000000`000003fd class foo *", or
// "0x00000000`000003fd "Hallo"", or check gdb formatting of characters.
static inline quint64 pointerValue(QString data)
{
    const int blankPos = data.indexOf(QLatin1Char(' '));
    if (blankPos != -1)
        data.truncate(blankPos);
    data.remove(QLatin1Char('`'));
    return data.toULongLong(0, 0);
}

// Return the type used for editing
static inline int editType(const WatchData &d)
{
    if (d.type == "bool")
        return QVariant::Bool;
    if (isIntType(d.type))
        return d.type.contains('u') ? QVariant::ULongLong : QVariant::LongLong;
    if (isFloatType(d.type))
        return QVariant::Double;
    // Check for pointers using hex values (0xAD00 "Hallo")
    if (isPointerType(d.type) && d.value.startsWith(QLatin1String("0x")))
        return QVariant::ULongLong;
   return QVariant::String;
}

// Convert to editable (see above)
static inline QVariant editValue(const WatchData &d)
{
    switch (editType(d)) {
    case QVariant::Bool:
        return d.value != QLatin1String("0") && d.value != QLatin1String("false");
    case QVariant::ULongLong:
        if (isPointerType(d.type)) // Fix pointer values (0xAD00 "Hallo" -> 0xAD00)
            return QVariant(pointerValue(d.value));
        return QVariant(d.value.toULongLong());
    case QVariant::LongLong:
        return QVariant(d.value.toLongLong());
    case QVariant::Double:
        return QVariant(d.value.toDouble());
    default:
        break;
    }
    // Some string value: '0x434 "Hallo"':
    // Remove quotes and replace newlines, which will cause line edit troubles.
    QString stringValue = d.value;
    if (stringValue.endsWith(QLatin1Char('"'))) {
        const int leadingDoubleQuote = stringValue.indexOf(QLatin1Char('"'));
        if (leadingDoubleQuote != stringValue.size() - 1) {
            stringValue.truncate(stringValue.size() - 1);
            stringValue.remove(0, leadingDoubleQuote + 1);
            stringValue.replace(QLatin1String("\n"), QLatin1String("\\n"));
        }
    }
    return QVariant(translate(stringValue));
}

bool WatchModel::canFetchMore(const QModelIndex &idx) const
{
    if (!idx.isValid())
        return false;
    WatchItem *item = watchItem(idx);
    QTC_ASSERT(item, return false);
    if (!contentIsValid() && !item->isInspect())
        return false;
    if (!item->iname.contains('.'))
        return false;
    return !m_fetchTriggered.contains(item->iname);
}

void WatchModel::fetchMore(const QModelIndex &idx)
{
    checkIndex(idx);
    if (!idx.isValid())
        return; // Triggered by ModelTester.
    WatchItem *item = watchItem(idx);
    QTC_ASSERT(item, return);
    QTC_ASSERT(!m_fetchTriggered.contains(item->iname), return);
    m_expandedINames.insert(item->iname);
    m_fetchTriggered.insert(item->iname);
    if (item->children.isEmpty()) {
        WatchData data = *item;
        data.setChildrenNeeded();
        WatchUpdateFlags flags;
        flags.tryIncremental = true;
        engine()->updateWatchData(data, flags);
    }
}

QModelIndex WatchModel::index(int row, int column, const QModelIndex &parent) const
{
    checkIndex(parent);
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    const WatchItem *item = watchItem(parent);
    QTC_ASSERT(item, return QModelIndex());
    if (row >= item->children.size())
        return QModelIndex();
    return createIndex(row, column, (void*)(item->children.at(row)));
}

QModelIndex WatchModel::parent(const QModelIndex &idx) const
{
    checkIndex(idx);
    if (!idx.isValid())
        return QModelIndex();

    const WatchItem *item = watchItem(idx);
    const WatchItem *parent = item->parent;
    if (!parent || parent == m_root)
        return QModelIndex();

    const WatchItem *grandparent = parent->parent;
    if (!grandparent)
        return QModelIndex();

    const WatchItems &uncles = grandparent->children;
    for (int i = 0, n = uncles.size(); i < n; ++i)
        if (uncles.at(i) == parent)
            return createIndex(i, 0, (void*) parent);

    return QModelIndex();
}

int WatchModel::rowCount(const QModelIndex &idx) const
{
    checkIndex(idx);
    if (!idx.isValid())
        return m_root->children.size();
    if (idx.column() > 0)
        return 0;
    return watchItem(idx)->children.size();
}

int WatchModel::columnCount(const QModelIndex &idx) const
{
    checkIndex(idx);
    return 3;
}

bool WatchModel::hasChildren(const QModelIndex &parent) const
{
    checkIndex(parent);
    WatchItem *item = watchItem(parent);
    return !item || item->hasChildren;
}

WatchItem *WatchModel::watchItem(const QModelIndex &idx) const
{
    checkIndex(idx);
    WatchItem *item = idx.isValid()
        ? static_cast<WatchItem*>(idx.internalPointer()) : m_root;
    CHECK(checkItem(item));
    return item;
}

QModelIndex WatchModel::watchIndex(const WatchItem *item) const
{
    CHECK(checkItem(item));
    return watchIndexHelper(item, m_root, QModelIndex());
}

QModelIndex WatchModel::watchIndexHelper(const WatchItem *needle,
    const WatchItem *parentItem, const QModelIndex &parentIndex) const
{
    checkIndex(parentIndex);
    if (needle == parentItem)
        return parentIndex;
    for (int i = parentItem->children.size(); --i >= 0; ) {
        const WatchItem *childItem = parentItem->children.at(i);
        QModelIndex childIndex = index(i, 0, parentIndex);
        QModelIndex idx = watchIndexHelper(needle, childItem, childIndex);
        checkIndex(idx);
        if (idx.isValid())
            return idx;
    }
    return QModelIndex();
}

void WatchModel::emitDataChanged(int column, const QModelIndex &parentIndex)
{
    checkIndex(parentIndex);
    QModelIndex idx1 = index(0, column, parentIndex);
    QModelIndex idx2 = index(rowCount(parentIndex) - 1, column, parentIndex);
    if (idx1.isValid() && idx2.isValid())
        emit dataChanged(idx1, idx2);
    //qDebug() << "CHANGING:\n" << idx1 << "\n" << idx2 << "\n"
    //    << data(parentIndex, INameRole).toString();
    checkIndex(idx1);
    checkIndex(idx2);
    for (int i = rowCount(parentIndex); --i >= 0; )
        emitDataChanged(column, index(i, 0, parentIndex));
}

void WatchModel::invalidateAll(const QModelIndex &parentIndex)
{
    checkIndex(parentIndex);
    QModelIndex idx1 = index(0, 0, parentIndex);
    QModelIndex idx2 = index(rowCount(parentIndex) - 1, columnCount(parentIndex) - 1, parentIndex);
    checkIndex(idx1);
    checkIndex(idx2);
    if (idx1.isValid() && idx2.isValid())
        emit dataChanged(idx1, idx2);
}

void WatchModel::resetValueCacheRecursively(WatchItem *item)
{
    m_valueCache[item->iname] = item->value;
    const WatchItems &items = item->children;
    for (int i = items.size(); --i >= 0; )
        resetValueCacheRecursively(items.at(i));
}

// Truncate value for item view, maintaining quotes.
static QString truncateValue(QString v)
{
    enum { maxLength = 512 };
    if (v.size() < maxLength)
        return v;
    const bool isQuoted = v.endsWith(QLatin1Char('"')); // check for 'char* "Hallo"'
    v.truncate(maxLength);
    v += isQuoted ? QLatin1String("...\"") : QLatin1String("...");
    return v;
}

int WatchModel::itemFormat(const WatchData &data) const
{
    const int individualFormat = theIndividualFormats.value(data.iname, AutomaticFormat);
    if (individualFormat != AutomaticFormat)
        return individualFormat;
    return theTypeFormats.value(stripForFormat(data.type), AutomaticFormat);
}

bool WatchModel::contentIsValid() const
{
    // FIXME:
    // inspector doesn't follow normal beginCycle()/endCycle()
    //if (m_type == InspectWatch)
    //    return true;
    return m_handler->m_contentsValid;
}

#if USE_EXPENSIVE_CHECKS
void WatchModel::checkTree()
{
    QSet<QByteArray> inames;
    checkTree(m_root, &inames);
    QSet<QByteArray> current = m_cache.keys().toSet();
    Q_ASSERT(inames == current);
}

void WatchModel::checkTree(WatchItem *item, QSet<QByteArray> *inames)
{
    checkItem(item);
    inames->insert(item->iname);
    for (int i = 0, n = item->children.size(); i != n; ++i)
        checkTree(item->children.at(i), inames);
}

void WatchModel::checkItem(const WatchItem *item) const
{
    Q_ASSERT(item->children.size() < 1000 * 1000);
    Q_ASSERT(m_cache2.contains(item));
    Q_ASSERT(m_cache2.value(item) == item->iname);
    Q_ASSERT(m_cache.value(item->iname) == item);
}
#endif

static QString expression(const WatchItem *item)
{
    if (!item->exp.isEmpty())
         return QString::fromLatin1(item->exp);
    if (item->address && !item->type.isEmpty()) {
        return QString::fromLatin1("*(%1*)%2").
                arg(QLatin1String(item->type), QLatin1String(item->hexAddress()));
    }
    if (const WatchItem *parent = item->parent) {
        if (!parent->exp.isEmpty())
           return QString::fromLatin1("(%1).%2")
            .arg(QString::fromLatin1(parent->exp), item->name);
    }
    return QString();
}

QString WatchModel::displayName(const WatchItem *item) const
{
    QString result;
    if (item->parent == m_returnRoot)
        result = tr("returned value");
    else if (item->name == QLatin1String("*"))
        result = QLatin1Char('*') + item->parent->name;
    else
        result = removeNamespaces(item->name);

    // Simplyfy names that refer to base classes.
    if (result.startsWith(QLatin1Char('['))) {
        result = simplifyType(result);
        if (result.size() > 30)
            result = result.left(27) + QLatin1String("...]");
    }

    return result;
}

QString WatchModel::displayValue(const WatchData &data) const
{
    QString result = removeNamespaces(truncateValue(formattedValue(data)));
    if (result.isEmpty() && data.address)
        result += QString::fromLatin1("@0x" + QByteArray::number(data.address, 16));
//    if (data.origaddr)
//        result += QString::fromLatin1(" (0x" + QByteArray::number(data.origaddr, 16) + ')');
    return result;
}

QString WatchModel::displayType(const WatchData &data) const
{
    QString result = data.displayedType.isEmpty()
        ? niceTypeHelper(data.type)
        : data.displayedType;
    if (data.bitsize)
        result += QString::fromLatin1(":%1").arg(data.bitsize);
    result.remove(QLatin1Char('\''));
    result = removeNamespaces(result);
    return result;
}

QVariant WatchModel::data(const QModelIndex &idx, int role) const
{
    checkIndex(idx);
    if (!idx.isValid())
        return QVariant(); // Triggered by ModelTester.

    const WatchItem *item = watchItem(idx);
    const WatchItem &data = *item;

    switch (role) {
        case LocalsEditTypeRole:
            return QVariant(editType(data));

        case LocalsNameRole:
            return QVariant(data.name);

        case LocalsIntegerBaseRole:
            if (isPointerType(data.type)) // Pointers using 0x-convention
                return QVariant(16);
            return QVariant(formatToIntegerBase(itemFormat(data)));

        case Qt::EditRole: {
            switch (idx.column()) {
                case 0:
                    return QVariant(expression(item));
                case 1:
                    return editValue(data);
                case 2:
                    // FIXME:: To be tested: Can debuggers handle those?
                    if (!data.displayedType.isEmpty())
                        return data.displayedType;
                    return QString::fromUtf8(data.type);
            }
        }

        case Qt::DisplayRole: {
            switch (idx.column()) {
                case 0:
                    return displayName(item);
                case 1:
                    return displayValue(data);
                case 2:
                    return displayType(data);
            }
        }

        case Qt::ToolTipRole:
            return boolSetting(UseToolTipsInLocalsView)
                ? data.toToolTip() : QVariant();

        case Qt::ForegroundRole: {
            static const QVariant red(QColor(200, 0, 0));
            static const QVariant gray(QColor(140, 140, 140));
            if (idx.column() == 1) {
                if (!data.valueEnabled)
                    return gray;
                if (!contentIsValid() && !data.isInspect())
                    return gray;
                if (data.value.isEmpty()) // This might still show 0x...
                    return gray;
                if (data.value != m_valueCache.value(data.iname))
                    return red;
            }
            break;
        }

        case LocalsExpressionRole:
            return QVariant(expression(item));

        case LocalsRawExpressionRole:
            return data.exp;

        case LocalsINameRole:
            return data.iname;

        case LocalsExpandedRole:
            return m_expandedINames.contains(data.iname);

        case LocalsTypeFormatListRole:
            return QVariant::fromValue(typeFormatList(data));

        case LocalsTypeRole:
            return removeNamespaces(displayType(data));

        case LocalsRawTypeRole:
            return QString::fromLatin1(data.type);

        case LocalsTypeFormatRole:
            return theTypeFormats.value(stripForFormat(data.type), AutomaticFormat);

        case LocalsIndividualFormatRole:
            return theIndividualFormats.value(data.iname, AutomaticFormat);

        case LocalsRawValueRole:
            return data.value;

        case LocalsObjectAddressRole:
            return data.address;

        case LocalsPointerAddressRole:
            return data.origaddr;

        case LocalsIsWatchpointAtObjectAddressRole: {
            BreakpointParameters bp(WatchpointAtAddress);
            bp.address = data.address;
            return engine()->breakHandler()->findWatchpoint(bp) != 0;
        }

        case LocalsSizeRole:
            return QVariant(data.size);

        case LocalsIsWatchpointAtPointerAddressRole:
            if (isPointerType(data.type)) {
                BreakpointParameters bp(WatchpointAtAddress);
                bp.address = pointerValue(data.value);
                return engine()->breakHandler()->findWatchpoint(bp) != 0;
            }
            return false;

        default:
            break;
    }
    return QVariant();
}

bool WatchModel::setData(const QModelIndex &idx, const QVariant &value, int role)
{
    checkIndex(idx);

    if (!idx.isValid())
        return false; // Triggered by ModelTester.

    WatchItem &data = *watchItem(idx);

    switch (role) {
        case Qt::EditRole:
            switch (idx.column()) {
            case 0: // Watch expression: See delegate.
                break;
            case 1: // Change value
                engine()->assignValueInDebugger(&data, expression(&data), value);
                break;
            case 2: // TODO: Implement change type.
                engine()->assignValueInDebugger(&data, expression(&data), value);
                break;
            }
        case LocalsExpandedRole:
            if (value.toBool()) {
                // Should already have been triggered by fetchMore()
                //QTC_CHECK(m_expandedINames.contains(data.iname));
                m_expandedINames.insert(data.iname);
            } else {
                m_expandedINames.remove(data.iname);
            }
            emit columnAdjustmentRequested();
            break;

        case LocalsTypeFormatRole:
            m_handler->setFormat(data.type, value.toInt());
            engine()->updateWatchData(data);
            break;

        case LocalsIndividualFormatRole: {
            const int format = value.toInt();
            if (format == AutomaticFormat)
                theIndividualFormats.remove(data.iname);
            else
                theIndividualFormats[data.iname] = format;
            engine()->updateWatchData(data);
            break;
        }
    }

    //emit dataChanged(idx, idx);
    return true;
}

Qt::ItemFlags WatchModel::flags(const QModelIndex &idx) const
{
    checkIndex(idx);
    if (!idx.isValid())
        return Qt::ItemFlags();

    WatchItem *item = watchItem(idx);
    QTC_ASSERT(item, return Qt::ItemFlags());
    const WatchData &data = *item;
    if (!contentIsValid() && !data.isInspect())
        return Qt::ItemFlags();

    // Enabled, editable, selectable, checkable, and can be used both as the
    // source of a drag and drop operation and as a drop target.

    static const Qt::ItemFlags notEditable
        = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    static const Qt::ItemFlags editable = notEditable | Qt::ItemIsEditable;

    // Disable editing if debuggee is positively running except for Inspector data
    const bool isRunning = engine() && engine()->state() == InferiorRunOk;
    if (isRunning && engine() && !engine()->hasCapability(AddWatcherWhileRunningCapability) &&
            !data.isInspect())
        return notEditable;

    if (data.isWatcher()) {
        if (idx.column() == 0 && data.iname.count('.') == 1)
            return editable; // Watcher names are editable.

        if (!data.name.isEmpty()) {
            // FIXME: Forcing types is not implemented yet.
            //if (idx.column() == 2)
            //    return editable; // Watcher types can be set by force.
            if (idx.column() == 1 && data.valueEditable)
                return editable; // Watcher values are sometimes editable.
        }
    } else if (data.isLocal()) {
        if (idx.column() == 1 && data.valueEditable)
            return editable; // Locals values are sometimes editable.
    } else if (data.isInspect()) {
        if (idx.column() == 1 && data.valueEditable)
            return editable; // Inspector values are sometimes editable.
    }
    return notEditable;
}

QVariant WatchModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Vertical)
        return QVariant();
    if (role == Qt::DisplayRole) {
        switch (section) {
            case 0: return QString(tr("Name")  + QLatin1String("     "));
            case 1: return QString(tr("Value") + QLatin1String("     "));
            case 2: return QString(tr("Type")  + QLatin1String("     "));
        }
    }
    return QVariant();
}

static inline QString msgArrayFormat(int n)
{
    return WatchModel::tr("Array of %n items", 0, n);
}

QString WatchModel::nameForFormat(int format)
{
    switch (format) {
        case RawFormat: return tr("Raw Data");
        case Latin1StringFormat: return tr("Latin1 String");
        case Utf8StringFormat: return tr("UTF-8 String");
        case Local8BitStringFormat: return tr("Local 8-Bit String");
        case Utf16StringFormat: return tr("UTF-16 String");
        case Ucs4StringFormat: return tr("UCS-4 String");
        case Array10Format: return msgArrayFormat(10);
        case Array100Format: return msgArrayFormat(100);
        case Array1000Format: return msgArrayFormat(1000);
        case Array10000Format: return msgArrayFormat(10000);
        case SeparateLatin1StringFormat: return tr("Latin1 String in Separate Window");
        case SeparateUtf8StringFormat: return tr("UTF-8 String in Separate Window");
        case DecimalIntegerFormat: return tr("Decimal Integer");
        case HexadecimalIntegerFormat: return tr("Hexadecimal Integer");
        case BinaryIntegerFormat: return tr("Binary Integer");
        case OctalIntegerFormat: return tr("Octal Integer");
        case CompactFloatFormat: return tr("Compact Float");
        case ScientificFloatFormat: return tr("Scientific Float");
    }

    QTC_CHECK(false);
    return QString();
}

TypeFormatList WatchModel::typeFormatList(const WatchData &data) const
{
    TypeFormatList formats;

    // Types supported by dumpers:
    // Hack: Compensate for namespaces.
    QString type = QLatin1String(stripForFormat(data.type));
    int pos = type.indexOf(QLatin1String("::Q"));
    if (pos >= 0 && type.count(QLatin1Char(':')) == 2)
        type.remove(0, pos + 2);
    pos = type.indexOf(QLatin1Char('<'));
    if (pos >= 0)
        type.truncate(pos);
    type.replace(QLatin1Char(':'), QLatin1Char('_'));
    QStringList reported = m_reportedTypeFormats.value(type);
    for (int i = 0, n = reported.size(); i != n; ++i)
        formats.append(TypeFormatItem(reported.at(i), i));

    // Fixed artificial string and pointer types.
    if (data.origaddr || isPointerType(data.type)) {
        formats.append(RawFormat);
        formats.append(Latin1StringFormat);
        formats.append(SeparateLatin1StringFormat);
        formats.append(Utf8StringFormat);
        formats.append(SeparateUtf8StringFormat);
        formats.append(Local8BitStringFormat);
        formats.append(Utf16StringFormat);
        formats.append(Ucs4StringFormat);
        formats.append(Array10Format);
        formats.append(Array100Format);
        formats.append(Array1000Format);
        formats.append(Array10000Format);
    } else if (data.type.contains("char[") || data.type.contains("char [")) {
        formats.append(Latin1StringFormat);
        formats.append(Utf8StringFormat);
        formats.append(Ucs4StringFormat);
    }

    // Fixed artificial floating point types.
    bool ok = false;
    data.value.toDouble(&ok);
    if (ok) {
        formats.append(CompactFloatFormat);
        formats.append(ScientificFloatFormat);
    }

    // Fixed artificial integral types.
    QString v = data.value;
    if (v.startsWith(QLatin1Char('-')))
        v = v.mid(1);
    v.toULongLong(&ok, 10);
    if (!ok)
        v.toULongLong(&ok, 16);
    if (!ok)
        v.toULongLong(&ok, 8);
    if (ok) {
        formats.append(DecimalIntegerFormat);
        formats.append(HexadecimalIntegerFormat);
        formats.append(BinaryIntegerFormat);
        formats.append(OctalIntegerFormat);
    }

    return formats;
}

// Determine sort order of watch items by sort order or alphabetical inames
// according to setting 'SortStructMembers'. We need a map key for insertBulkData
// and a predicate for finding the insertion position of a single item.

// Set this before using any of the below according to action
static bool sortWatchDataAlphabetically = true;

static bool watchDataLessThan(const QByteArray &iname1, int sortId1,
    const QByteArray &iname2, int sortId2)
{
    if (!sortWatchDataAlphabetically)
        return sortId1 < sortId2;
    // Get positions of last part of iname 'local.this.i1" -> "i1"
    int cmpPos1 = iname1.lastIndexOf('.');
    if (cmpPos1 == -1)
        cmpPos1 = 0;
    else
        cmpPos1++;
    int cmpPos2 = iname2.lastIndexOf('.');
    if (cmpPos2 == -1)
        cmpPos2 = 0;
    else
        cmpPos2++;
    // Are we looking at an array with numerical inames 'local.this.i1.0" ->
    // Go by sort id.
    if (cmpPos1 < iname1.size() && cmpPos2 < iname2.size()
            && isdigit(iname1.at(cmpPos1)) && isdigit(iname2.at(cmpPos2)))
        return sortId1 < sortId2;
    // Alphabetically
    return qstrcmp(iname1.constData() + cmpPos1, iname2.constData() + cmpPos2) < 0;
}

// Sort key for watch data consisting of iname and numerical sort id.
struct WatchDataSortKey
{
    explicit WatchDataSortKey(const WatchData &wd)
        : iname(wd.iname), sortId(wd.sortId) {}
    QByteArray iname;
    int sortId;
};

inline bool operator<(const WatchDataSortKey &k1, const WatchDataSortKey &k2)
{
    return watchDataLessThan(k1.iname, k1.sortId, k2.iname, k2.sortId);
}

bool watchItemSorter(const WatchItem *item1, const WatchItem *item2)
{
    return watchDataLessThan(item1->iname, item1->sortId, item2->iname, item2->sortId);
}

static int findInsertPosition(const QList<WatchItem *> &list, const WatchItem *item)
{
    sortWatchDataAlphabetically = boolSetting(SortStructMembers);
    const QList<WatchItem *>::const_iterator it =
        qLowerBound(list.begin(), list.end(), item, watchItemSorter);
    return it - list.begin();
}

void WatchModel::insertDataItem(const WatchData &data, bool destructive)
{
#if USE_WATCH_MODEL_TEST
    (void) new ModelTest(this, this);
#endif
    m_fetchTriggered.remove(data.iname);
    CHECK(checkTree());

    QTC_ASSERT(!data.iname.isEmpty(), qDebug() << data.toString(); return);

    if (WatchItem *item = findItem(data.iname)) {
        // Remove old children.
        if (destructive)
            destroyChildren(item);

        // Overwrite old entry.
        assignData(item, data);
        QModelIndex idx = watchIndex(item);
        checkIndex(idx);
        emit dataChanged(idx, idx.sibling(idx.row(), 2));
    } else {
        // Add new entry.
        WatchItem *parent = findItem(parentName(data.iname));
        QTC_ASSERT(parent, return);
        WatchItem *newItem = createItem(data);
        newItem->parent = parent;
        const int row = findInsertPosition(parent->children, newItem);
        QModelIndex idx = watchIndex(parent);
        checkIndex(idx);
        beginInsertRows(idx, row, row);
        parent->children.insert(row, newItem);
        endInsertRows();
        if (m_expandedINames.contains(parent->iname))
            emit itemIsExpanded(idx);
    }
}

// Identify items that have to be removed, i.e. current items that
// have an ancestor in the list, but do not appear in the list themselves.
bool WatchModel::ancestorChanged(const QSet<QByteArray> &inames, WatchItem *item) const
{
    if (item == m_root)
        return false;
    WatchItem *parent = item->parent;
    if (inames.contains(parent->iname))
        return true;
    return ancestorChanged(inames, parent);
}

void WatchModel::insertBulkData(const QList<WatchData> &list)
{
#if 1
    for (int i = 0, n = list.size(); i != n; ++i) {
        const WatchData &data = list.at(i);
        insertDataItem(data, true);
        m_handler->showEditValue(data);
    }
#else
    // Destroy unneeded items.
    QSet<QByteArray> inames;
    for (int i = list.size(); --i >= 0; )
        inames.insert(list.at(i).iname);

    QList<QByteArray> toDestroy;
    for (Cache::const_iterator it = m_cache.begin(), et = m_cache.end(); it != et; ++it)
        if (!inames.contains(it.key()) && ancestorChanged(inames, it.value()))
            toDestroy.append(it.key());

    for (int i = 0, n = toDestroy.size(); i != n; ++i) {
        // Can be destroyed as child of a previous item.
        WatchItem *item = findItem(toDestroy.at(i));
        if (item)
            destroyItem(item);
    }

    // All remaining items are changed or new.
    for (int i = 0, n = list.size(); i != n; ++i)
        insertDataItem(list.at(i), false);
#endif
    CHECK(checkTree());
    emit columnAdjustmentRequested();
}

static void debugRecursion(QDebug &d, const WatchItem *item, int depth)
{
    d << QString(2 * depth, QLatin1Char(' ')) << item->toString() << '\n';
    foreach (const WatchItem *child, item->children)
        debugRecursion(d, child, depth + 1);
}

QDebug operator<<(QDebug d, const WatchModel &m)
{
    QDebug nospace = d.nospace();
    if (m.m_root)
        debugRecursion(nospace, m.m_root, 0);
    return d;
}

void WatchModel::formatRequests(QByteArray *out, const WatchItem *item) const
{
    int format = theIndividualFormats.value(item->iname, AutomaticFormat);
    if (format == AutomaticFormat)
        format = theTypeFormats.value(stripForFormat(item->type), AutomaticFormat);
    if (format != AutomaticFormat)
        *out += item->iname + ":format=" + QByteArray::number(format) + ',';
    foreach (const WatchItem *child, item->children)
        formatRequests(out, child);
}

void WatchModel::showInEditorHelper(QString *contents, WatchItem *item, int depth)
{
    const QChar tab = QLatin1Char('\t');
    const QChar nl = QLatin1Char('\n');
    contents->append(QString(depth, tab));
    contents->append(item->name);
    contents->append(tab);
    contents->append(item->value);
    contents->append(tab);
    contents->append(QString::fromLatin1(item->type));
    contents->append(nl);
    foreach (WatchItem *child, item->children)
       showInEditorHelper(contents, child, depth + 1);
}

void WatchModel::setCurrentItem(const QByteArray &iname)
{
    if (WatchItem *item = findItem(iname)) {
        QModelIndex idx = watchIndex(item);
        checkIndex(idx);
        emit currentIndexRequested(idx);
    }
}

///////////////////////////////////////////////////////////////////////
//
// WatchHandler
//
///////////////////////////////////////////////////////////////////////

WatchHandler::WatchHandler(DebuggerEngine *engine)
{
    m_engine = engine;
    m_watcherCounter = sessionValue("Watchers").toStringList().count();
    m_model = new WatchModel(this);
    m_contentsValid = false;
    m_contentsValid = true; // FIXME
    m_resetLocationScheduled = false;
    m_separatedView = new SeparatedView;
}

WatchHandler::~WatchHandler()
{
    delete m_separatedView;
    m_separatedView = 0;
    // Do it manually to prevent calling back in model destructors
    // after m_cache is destroyed.
    delete m_model;
    m_model = 0;
}

void WatchHandler::cleanup()
{
    m_model->m_expandedINames.clear();
    theWatcherNames.remove(QByteArray());
    m_model->reinitialize();
    m_model->m_fetchTriggered.clear();
    m_separatedView->hide();
}

void WatchHandler::insertIncompleteData(const WatchData &data)
{
    MODEL_DEBUG("INSERTDATA: " << data.toString());
    if (!data.isValid()) {
        qWarning("%s:%d: Attempt to insert invalid watch item: %s",
            __FILE__, __LINE__, qPrintable(data.toString()));
        return;
    }

    if (data.isSomethingNeeded() && data.iname.contains('.')) {
        MODEL_DEBUG("SOMETHING NEEDED: " << data.toString());
        if (!m_engine->isSynchronous() || data.isInspect()) {
            m_model->insertDataItem(data, true);
            m_engine->updateWatchData(data);
        } else {
            m_engine->showMessage(QLatin1String("ENDLESS LOOP: SOMETHING NEEDED: ")
                + data.toString());
            WatchData data1 = data;
            data1.setAllUnneeded();
            data1.setValue(QLatin1String("<unavailable synchronous data>"));
            data1.setHasChildren(false);
            m_model->insertDataItem(data1, true);
        }
    } else {
        MODEL_DEBUG("NOTHING NEEDED: " << data.toString());
        m_model->insertDataItem(data, true);
        showEditValue(data);
    }
}

void WatchHandler::insertData(const WatchData &data)
{
    QList<WatchData> list;
    list.append(data);
    insertData(list);
}

void WatchHandler::insertData(const QList<WatchData> &list)
{
    m_model->insertBulkData(list);

    m_contentsValid = true;
    updateWatchersWindow();
}

void WatchHandler::removeAllData(bool includeInspectData)
{
    m_model->reinitialize(includeInspectData);
    updateWatchersWindow();
}

void WatchHandler::resetValueCache()
{
    m_model->m_valueCache.clear();
    m_model->resetValueCacheRecursively(m_model->m_root);
}

void WatchHandler::removeData(const QByteArray &iname)
{
    WatchItem *item = m_model->findItem(iname);
    if (!item)
        return;
    if (item->isWatcher()) {
        theWatcherNames.remove(item->exp);
        saveWatchers();
    }
    m_model->destroyItem(item);
    updateWatchersWindow();
}

void WatchHandler::removeChildren(const QByteArray &iname)
{
    WatchItem *item = m_model->findItem(iname);
    if (item)
        m_model->destroyChildren(item);
    updateWatchersWindow();
}

QByteArray WatchHandler::watcherName(const QByteArray &exp)
{
    return "watch." + QByteArray::number(theWatcherNames[exp]);
}

void WatchHandler::watchExpression(const QString &exp0, const QString &name)
{
    QString exp = exp0;

    QTC_ASSERT(m_engine, return);
    // Do not insert the same entry more then once.
    if (theWatcherNames.value(exp.toLatin1()))
        return;

    // FIXME: 'exp' can contain illegal characters
    exp.replace(QLatin1Char('#'), QString());

    WatchData data;
    data.exp = exp.toLatin1();
    data.name = name.isEmpty() ? exp : name;
    theWatcherNames[data.exp] = m_watcherCounter++;
    saveWatchers();

    if (exp.isEmpty())
        data.setAllUnneeded();
    data.iname = watcherName(data.exp);
    if (m_engine->state() == DebuggerNotReady) {
        data.setAllUnneeded();
        data.setValue(QString(QLatin1Char(' ')));
        data.setHasChildren(false);
        insertIncompleteData(data);
    } else if (m_engine->isSynchronous()) {
        m_engine->updateWatchData(data);
    } else {
        insertIncompleteData(data);
    }
    updateWatchersWindow();
}

// Watch something obtained from the editor.
// Prefer to watch an existing local variable by its expression
// (address) if it can be found. Default to watchExpression().
void WatchHandler::watchVariable(const QString &exp)
{
    if (const WatchData *localVariable = findCppLocalVariable(exp))
        watchExpression(QLatin1String(localVariable->exp), exp);
    else
        watchExpression(exp);
}

static void swapEndian(char *d, int nchar)
{
    QTC_ASSERT(nchar % 4 == 0, return);
    for (int i = 0; i < nchar; i += 4) {
        char c = d[i];
        d[i] = d[i + 3];
        d[i + 3] = c;
        c = d[i + 1];
        d[i + 1] = d[i + 2];
        d[i + 2] = c;
    }
}

void WatchHandler::showEditValue(const WatchData &data)
{
    const QByteArray key  = data.address ? data.hexAddress() : data.iname;
    switch (data.editformat) {
    case StopDisplay:
        m_separatedView->removeObject(data.iname);
        break;
    case DisplayImageData:
    case DisplayImageFile: {  // QImage
        int width = 0, height = 0, nbytes = 0, format = 0;
        QByteArray ba;
        uchar *bits = 0;
        if (data.editformat == DisplayImageData) {
            ba = QByteArray::fromHex(data.editvalue);
            QTC_ASSERT(ba.size() > 16, return);
            const int *header = (int *)(ba.data());
            if (!ba.at(0) && !ba.at(1)) // Check on 'width' for Python dumpers returning 4-byte swapped-data.
                swapEndian(ba.data(), 16);
            bits = 16 + (uchar *)(ba.data());
            width = header[0];
            height = header[1];
            nbytes = header[2];
            format = header[3];
        } else if (data.editformat == DisplayImageFile) {
            QTextStream ts(data.editvalue);
            QString fileName;
            ts >> width >> height >> nbytes >> format >> fileName;
            QFile f(fileName);
            f.open(QIODevice::ReadOnly);
            ba = f.readAll();
            bits = (uchar*)ba.data();
            nbytes = width * height;
        }
        QTC_ASSERT(0 < width && width < 10000, return);
        QTC_ASSERT(0 < height && height < 10000, return);
        QTC_ASSERT(0 < nbytes && nbytes < 10000 * 10000, return);
        QTC_ASSERT(0 < format && format < 32, return);
        QImage im(width, height, QImage::Format(format));
        std::memcpy(im.bits(), bits, nbytes);
        const QString title = data.address ?
            tr("%1 Object at %2").arg(QLatin1String(data.type),
                QLatin1String(data.hexAddress())) :
            tr("%1 Object at Unknown Address").arg(QLatin1String(data.type));
        ImageViewer *v = m_separatedView->prepareObject<ImageViewer>(key, title);
        v->setProperty(INameProperty, data.iname);
        v->setImage(im);
        break;
    }
    case DisplayUtf16String:
    case DisplayLatin1String:
    case DisplayUtf8String: { // String data.
        QByteArray ba = QByteArray::fromHex(data.editvalue);
        QString str;
        if (data.editformat == DisplayUtf16String)
            str = QString::fromUtf16((ushort *)ba.constData(), ba.size()/2);
        else if (data.editformat == DisplayLatin1String)
            str = QString::fromLatin1(ba.constData(), ba.size());
        else if (data.editformat == DisplayUtf8String)
            str = QString::fromUtf8(ba.constData(), ba.size());
        QTextEdit *t = m_separatedView->prepareObject<QTextEdit>(key, data.name);
        t->setProperty(INameProperty, data.iname);
        t->setText(str);
        break;
    }
    default:
        QTC_ASSERT(false, qDebug() << "Display format: " << data.editformat);
        break;
    }
}

void WatchHandler::clearWatches()
{
    if (theWatcherNames.isEmpty())
        return;
    m_model->destroyChildren(m_model->m_watchRoot);
    theWatcherNames.clear();
    m_watcherCounter = 0;
    updateWatchersWindow();
    saveWatchers();
}

void WatchHandler::updateWatchersWindow()
{
    // Force show/hide of watchers and return view.
    static int previousShowWatch = -1;
    static int previousShowReturn = -1;
    int showWatch = !m_model->m_watchRoot->children.isEmpty();
    int showReturn = !m_model->m_returnRoot->children.isEmpty();
    if (showWatch == previousShowWatch && showReturn == previousShowReturn)
        return;
    previousShowWatch = showWatch;
    previousShowReturn = showReturn;
    debuggerCore()->updateWatchersWindow(showWatch, showReturn);
}

QStringList WatchHandler::watchedExpressions()
{
    // Filter out invalid watchers.
    QStringList watcherNames;
    QHashIterator<QByteArray, int> it(theWatcherNames);
    while (it.hasNext()) {
        it.next();
        const QByteArray &watcherName = it.key();
        if (!watcherName.isEmpty())
            watcherNames.push_back(QLatin1String(watcherName));
    }
    return watcherNames;
}

void WatchHandler::saveWatchers()
{
    setSessionValue("Watchers", watchedExpressions());
}

void WatchHandler::loadFormats()
{
    QVariant value = sessionValue("DefaultFormats");
    QMapIterator<QString, QVariant> it(value.toMap());
    while (it.hasNext()) {
        it.next();
        if (!it.key().isEmpty())
            theTypeFormats.insert(it.key().toUtf8(), it.value().toInt());
    }

    value = sessionValue("IndividualFormats");
    it = QMapIterator<QString, QVariant>(value.toMap());
    while (it.hasNext()) {
        it.next();
        if (!it.key().isEmpty())
            theIndividualFormats.insert(it.key().toUtf8(), it.value().toInt());
    }
}

void WatchHandler::saveFormats()
{
    QMap<QString, QVariant> formats;
    QHashIterator<QByteArray, int> it(theTypeFormats);
    while (it.hasNext()) {
        it.next();
        const int format = it.value();
        if (format != AutomaticFormat) {
            const QByteArray key = it.key().trimmed();
            if (!key.isEmpty())
                formats.insert(QString::fromLatin1(key), format);
        }
    }
    setSessionValue("DefaultFormats", formats);

    formats.clear();
    it = QHashIterator<QByteArray, int>(theIndividualFormats);
    while (it.hasNext()) {
        it.next();
        const int format = it.value();
        const QByteArray key = it.key().trimmed();
        if (!key.isEmpty())
            formats.insert(QString::fromLatin1(key), format);
    }
    setSessionValue("IndividualFormats", formats);
}

void WatchHandler::saveSessionData()
{
    saveWatchers();
    saveFormats();
}

void WatchHandler::loadSessionData()
{
    loadFormats();
    theWatcherNames.clear();
    m_watcherCounter = 0;
    QVariant value = sessionValue("Watchers");
    m_model->destroyChildren(m_model->m_watchRoot);
    foreach (const QString &exp, value.toStringList())
        watchExpression(exp);
}

QAbstractItemModel *WatchHandler::model() const
{
    return m_model;
}

const WatchData *WatchHandler::watchData(const QModelIndex &idx) const
{
    return m_model->watchItem(idx);
}

void WatchHandler::fetchMore(const QByteArray &iname) const
{
    QModelIndex idx = m_model->watchIndex(m_model->findItem(iname));
    m_model->checkIndex(idx);
    model()->fetchMore(idx);
}

const WatchData *WatchHandler::findData(const QByteArray &iname) const
{
    return m_model->findItem(iname);
}

const WatchData *WatchHandler::findCppLocalVariable(const QString &name) const
{
    // Can this be found as a local variable?
    const QByteArray localsPrefix("local.");
    QByteArray iname = localsPrefix + name.toLatin1();
    if (const WatchData *wd = findData(iname))
        return wd;
    // Nope, try a 'local.this.m_foo'.
    iname.insert(localsPrefix.size(), "this.");
    if (const WatchData *wd = findData(iname))
        return wd;
    return 0;
}

bool WatchHandler::hasItem(const QByteArray &iname) const
{
    return m_model->findItem(iname);
}

void WatchHandler::setFormat(const QByteArray &type0, int format)
{
    const QByteArray type = stripForFormat(type0);
    if (format == AutomaticFormat)
        theTypeFormats.remove(type);
    else
        theTypeFormats[type] = format;
    saveFormats();
    m_model->emitDataChanged(1);
}

int WatchHandler::format(const QByteArray &iname) const
{
    int result = AutomaticFormat;
    if (const WatchData *item = m_model->findItem(iname)) {
        int result = theIndividualFormats.value(item->iname, AutomaticFormat);
        if (result == AutomaticFormat)
            result = theTypeFormats.value(stripForFormat(item->type), AutomaticFormat);
    }
    return result;
}

QByteArray WatchHandler::expansionRequests() const
{
    QByteArray ba;
    m_model->formatRequests(&ba, m_model->m_root);
    if (!m_model->m_expandedINames.isEmpty()) {
        QSetIterator<QByteArray> jt(m_model->m_expandedINames);
        while (jt.hasNext()) {
            QByteArray iname = jt.next();
            ba.append(iname);
            ba.append(',');
        }
        ba.chop(1);
    }
    return ba;
}

QByteArray WatchHandler::typeFormatRequests() const
{
    QByteArray ba;
    if (!theTypeFormats.isEmpty()) {
        QHashIterator<QByteArray, int> it(theTypeFormats);
        while (it.hasNext()) {
            it.next();
            const int format = it.value();
            if (format >= RawFormat && format < ArtificialFormatBase) {
                ba.append(it.key().toHex());
                ba.append('=');
                ba.append(QByteArray::number(format));
                ba.append(',');
            }
        }
        ba.chop(1);
    }
    return ba;
}

QByteArray WatchHandler::individualFormatRequests() const
{
    QByteArray ba;
    if (!theIndividualFormats.isEmpty()) {
        QHashIterator<QByteArray, int> it(theIndividualFormats);
        while (it.hasNext()) {
            it.next();
            const int format = it.value();
            if (format >= RawFormat && format < ArtificialFormatBase) {
                ba.append(it.key());
                ba.append('=');
                ba.append(QByteArray::number(it.value()));
                ba.append(',');
            }
        }
        ba.chop(1);
    }
    return ba;
}

void WatchHandler::addTypeFormats(const QByteArray &type, const QStringList &formats)
{
    m_model->m_reportedTypeFormats.insert(QLatin1String(stripForFormat(type)), formats);
}

QString WatchHandler::editorContents()
{
    QString contents;
    m_model->showInEditorHelper(&contents, m_model->m_root, 0);
    return contents;
}

void WatchHandler::setTypeFormats(const DumperTypeFormats &typeFormats)
{
    m_model->m_reportedTypeFormats = typeFormats;
}

DumperTypeFormats WatchHandler::typeFormats() const
{
    return m_model->m_reportedTypeFormats;
}

void WatchHandler::editTypeFormats(bool includeLocals, const QByteArray &iname)
{
    Q_UNUSED(includeLocals);
    TypeFormatsDialog dlg(0);

    //QHashIterator<QString, QStringList> it(m_reportedTypeFormats);
    QList<QString> l = m_model->m_reportedTypeFormats.keys();
    Utils::sort(l);
    foreach (const QString &ba, l) {
        int f = iname.isEmpty() ? AutomaticFormat : format(iname);
        dlg.addTypeFormats(ba, m_model->m_reportedTypeFormats.value(ba), f);
    }
    if (dlg.exec())
        setTypeFormats(dlg.typeFormats());
}

void WatchHandler::scheduleResetLocation()
{
    m_contentsValid = false;
    //m_contentsValid = true; // FIXME
    m_resetLocationScheduled = true;
}

void WatchHandler::resetLocation()
{
    if (m_resetLocationScheduled) {
        m_resetLocationScheduled = false;
        //m_model->invalidateAll();  FIXME
    }
}

bool WatchHandler::isValidToolTip(const QByteArray &iname) const
{
    WatchItem *item = m_model->findItem(iname);
    return item && !item->type.trimmed().isEmpty();
}

void WatchHandler::setCurrentItem(const QByteArray &iname)
{
    m_model->setCurrentItem(iname);
}

QHash<QByteArray, int> WatchHandler::watcherNames()
{
    return theWatcherNames;
}

void WatchHandler::setUnprintableBase(int base)
{
    theUnprintableBase = base;
    m_model->emitAllChanged();
}

int WatchHandler::unprintableBase()
{
    return theUnprintableBase;
}

bool WatchHandler::isExpandedIName(const QByteArray &iname) const
{
    return m_model->m_expandedINames.contains(iname);
}

QSet<QByteArray> WatchHandler::expandedINames() const
{
    return m_model->m_expandedINames;
}


////////////////////////////////////////////////////////////////////
//
// TypeFormatItem/List
//
////////////////////////////////////////////////////////////////////

TypeFormatItem::TypeFormatItem(const QString &display, int format)
    : display(display), format(format)
{}

void TypeFormatList::append(int format)
{
    append(TypeFormatItem(WatchModel::nameForFormat(format), format));
}

TypeFormatItem TypeFormatList::find(int format) const
{
    for (int i = 0; i != size(); ++i)
        if (at(i).format == format)
            return at(i);
    return TypeFormatItem();
}

} // namespace Internal
} // namespace Debugger

#include "watchhandler.moc"
