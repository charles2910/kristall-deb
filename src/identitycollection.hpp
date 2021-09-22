#ifndef IDENTITYCOLLECTION_HPP
#define IDENTITYCOLLECTION_HPP

#include "cryptoidentity.hpp"

#include <QAbstractItemModel>
#include <memory>
#include <QSettings>

class IdentityCollection : public QAbstractItemModel
{
    Q_OBJECT
    struct Node  {
        enum Type { Root, Group, Identity };
        Node * parent = nullptr;
        int index = 0;
        std::vector<std::unique_ptr<Node>> children;
        Type type;
        explicit Node(Type t) : type(t) { }
        virtual ~Node() = default;

        template<typename T>
        T & as() { return *static_cast<T*>(this); }

        template<typename T>
        T const & as() const { return *static_cast<T const*>(this); }
    };

    struct IdentityNode : Node {
        CryptoIdentity identity;
        IdentityNode() : Node(Identity) { }
        ~IdentityNode() override = default;
    };

    struct GroupNode : Node {
        QString title;
        GroupNode() : Node(Group) { }
        ~GroupNode() override = default;
    };

    struct RootNode : Node {
        RootNode() : Node(Root) { }
        ~RootNode() override = default;
    };

public:
    explicit IdentityCollection(QObject *parent = nullptr);

    IdentityCollection(IdentityCollection const & other);

    IdentityCollection & operator=(IdentityCollection const &);
    IdentityCollection & operator=(IdentityCollection &&);

public:
    void load(QSettings & settings);

    void save(QSettings & settings) const;

    bool addGroup(QString const & group);

    bool addCertificate(QString const & group, CryptoIdentity const & id);

    CryptoIdentity getIdentity(QModelIndex const & index) const;

    CryptoIdentity * getMutableIdentity(QModelIndex const & index);

    QStringList groups() const;

    //! Returns the group name of the index.
    QString group(QModelIndex const & index) const;

    bool destroyIdentity(QModelIndex const & index);

    bool canDeleteGroup(QString const & group_name);
    bool deleteGroup(QString const & group_name);

    //! Returns a list of non-mutable references to all contained identities
    QVector<CryptoIdentity const *> allIdentities() const;

public:
    // Header:
    // QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Basic functionality:
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // Drag'n'Drop

    Qt::ItemFlags flags(const QModelIndex &index) const override;

    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;
    Qt::DropActions supportedDropActions() const override;

    Qt::DropActions supportedDragActions() const override;

    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;


private:
    void relayout();

    bool internalAddGroup(QString const & group_name, GroupNode * & out_group);

private:
    RootNode root;
};

#endif // IDENTITYCOLLECTION_HPP
