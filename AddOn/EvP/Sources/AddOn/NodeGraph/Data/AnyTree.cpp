#include "NodeGraph/Data/AnyTree.hpp"

#include <variant>

namespace evp::nodegraph::data {
namespace {

// One switch over the item vocabulary, written ONCE. `fn` is a template lambda
// invoked with the C++ type behind the enum, so every erased operation below is
// its typed counterpart plus a cast, and adding an item type breaks compilation
// here rather than silently skipping a case somewhere else.
template <class Fn> auto DispatchItemType (ItemType type, Fn&& fn)
{
    switch (type) {
        case ItemType::Bool:
            return fn.template operator()<bool> ();
        case ItemType::Integer:
            return fn.template operator()<int64_t> ();
        case ItemType::Double:
            return fn.template operator()<double> ();
        case ItemType::String:
            return fn.template operator()<std::string> ();
        case ItemType::Point3:
            return fn.template operator()<Point3> ();
        case ItemType::Polyline:
            return fn.template operator()<Polyline> ();
        case ItemType::Polygon:
            return fn.template operator()<Polygon> ();
        case ItemType::Mesh:
            return fn.template operator()<Value::ImmutableMesh> ();
        case ItemType::ElementRef:
            return fn.template operator()<ArchicadElementRef> ();
        case ItemType::Any:
            break;
    }
    return fn.template operator()<Value> ();
}

// An Any tree stores the erased Value itself; every other tree stores the item.
template <class T> bool ItemFromValue (const Value& value, T& result)
{
    const T* held = std::get_if<T> (&value.DataValue ());
    if (held == nullptr)
        return false;
    result = *held;
    return true;
}

template <> bool ItemFromValue<Value> (const Value& value, Value& result)
{
    if (!IsAtomicValue (value))
        return false;
    result = value;
    return true;
}

template <class T> const DataTree<T>& AsTyped (const IDataTree& tree)
{
    return static_cast<const DataTree<T>&> (tree);
}

} // namespace

struct AnyTreeBuilder::Impl {
    std::variant<DataTreeBuilder<bool>, DataTreeBuilder<int64_t>, DataTreeBuilder<double>, DataTreeBuilder<std::string>,
                 DataTreeBuilder<Point3>, DataTreeBuilder<Polyline>, DataTreeBuilder<Polygon>,
                 DataTreeBuilder<Value::ImmutableMesh>, DataTreeBuilder<ArchicadElementRef>, DataTreeBuilder<Value>>
        builder;
};

namespace {

// The variant alternative that stores items of `type`.
size_t BuilderIndex (ItemType type)
{
    switch (type) {
        case ItemType::Bool:
            return 0;
        case ItemType::Integer:
            return 1;
        case ItemType::Double:
            return 2;
        case ItemType::String:
            return 3;
        case ItemType::Point3:
            return 4;
        case ItemType::Polyline:
            return 5;
        case ItemType::Polygon:
            return 6;
        case ItemType::Mesh:
            return 7;
        case ItemType::ElementRef:
            return 8;
        case ItemType::Any:
            break;
    }
    return 9;
}

} // namespace

AnyTreeBuilder::AnyTreeBuilder (ItemType itemType) : itemType_ (itemType), impl_ (std::make_unique<Impl> ())
{
    // Select the alternative that matches the declared type. Done by index
    // rather than by emplace<T> so the mapping lives in exactly one function.
    switch (BuilderIndex (itemType)) {
        case 0:
            impl_->builder.emplace<DataTreeBuilder<bool>> ();
            break;
        case 1:
            impl_->builder.emplace<DataTreeBuilder<int64_t>> ();
            break;
        case 2:
            impl_->builder.emplace<DataTreeBuilder<double>> ();
            break;
        case 3:
            impl_->builder.emplace<DataTreeBuilder<std::string>> ();
            break;
        case 4:
            impl_->builder.emplace<DataTreeBuilder<Point3>> ();
            break;
        case 5:
            impl_->builder.emplace<DataTreeBuilder<Polyline>> ();
            break;
        case 6:
            impl_->builder.emplace<DataTreeBuilder<Polygon>> ();
            break;
        case 7:
            impl_->builder.emplace<DataTreeBuilder<Value::ImmutableMesh>> ();
            break;
        case 8:
            impl_->builder.emplace<DataTreeBuilder<ArchicadElementRef>> ();
            break;
        default:
            impl_->builder.emplace<DataTreeBuilder<Value>> ();
            break;
    }
}

AnyTreeBuilder::~AnyTreeBuilder () = default;
AnyTreeBuilder::AnyTreeBuilder (AnyTreeBuilder&&) noexcept = default;
AnyTreeBuilder& AnyTreeBuilder::operator= (AnyTreeBuilder&&) noexcept = default;

bool AnyTreeBuilder::Add (const DataPath& path, const Value& value, SharedMetadata metadata, std::string& error)
{
    bool ok = false;
    std::visit (
        [&] (auto& builder) {
            using T = typename std::decay_t<decltype (builder)>::Item;
            T item {};
            if (!ItemFromValue<T> (value, item))
                return;
            builder.Add (path, std::move (item), std::move (metadata));
            ok = true;
        },
        impl_->builder);

    if (!ok) {
        if (value.Type () == ValueType::List) {
            // Not a type mismatch but a shape one, and worth its own sentence:
            // collection shape belongs to the tree and nowhere else (7.2), so
            // there is no depth to limit - a nested list simply cannot be an
            // item, at any depth.
            error = path.ToString () + ": a list is not an item; a tree holds collection shape itself";
        }
        else {
            const std::optional<ItemType> arrived = ItemTypeFromValueType (value.Type ());
            error = path.ToString () + ": expected an item of type '" + ItemTypeName (itemType_) + "', got '" +
                    (arrived.has_value () ? ItemTypeName (*arrived) : "none") + "'";
        }
    }
    return ok;
}

void AnyTreeBuilder::AddNull (const DataPath& path, SharedMetadata metadata)
{
    std::visit ([&] (auto& builder) { builder.AddNull (path, std::move (metadata)); }, impl_->builder);
}

void AnyTreeBuilder::EnsureList (const DataPath& path)
{
    std::visit ([&] (auto& builder) { builder.EnsureList (path); }, impl_->builder);
}

bool AnyTreeBuilder::AddList (const DataPath& path, const IDataList& list, std::string& error)
{
    if (list.Type () != itemType_) {
        error = path.ToString () + ": expected a list of '" + ItemTypeName (itemType_) + "', got '" +
                ItemTypeName (list.Type ()) + "'";
        return false;
    }

    EnsureList (path);
    for (size_t index = 0; index < list.Size (); ++index) {
        const std::optional<Value> value = list.ValueAt (index);
        if (!value.has_value ()) {
            AddNull (path, list.MetadataAt (index));
            continue;
        }
        if (!Add (path, *value, list.MetadataAt (index), error))
            return false;
    }
    return true;
}

TreeValue AnyTreeBuilder::Finish () &&
{
    TreeValue result;
    result.itemType = itemType_;
    std::visit ([&] (auto& builder) { result.tree = std::move (builder).Finish (); }, impl_->builder);
    return result;
}

TreeValue EmptyTreeValue (ItemType itemType)
{
    return AnyTreeBuilder (itemType).Finish ();
}

bool MergeTreeValues (const std::vector<TreeValue>& trees, const FanInContract& contract, TreeValue& result,
                      std::string& error)
{
    if (trees.empty ()) {
        error = "A fan-in needs at least one tree";
        return false;
    }
    for (const TreeValue& tree : trees) {
        if (!tree.IsPresent ()) {
            error = "A fan-in input has no tree";
            return false;
        }
        if (tree.itemType != trees.front ().itemType) {
            error = std::string ("A fan-in mixes item types: '") + ItemTypeName (trees.front ().itemType) + "' and '" +
                    ItemTypeName (tree.itemType) + "'";
            return false;
        }
    }
    if (trees.size () == 1) {
        result = trees.front ();
        return true;
    }

    return DispatchItemType (trees.front ().itemType, [&]<class T> () {
        std::shared_ptr<const DataTree<T>> combined = std::static_pointer_cast<const DataTree<T>> (trees.front ().tree);
        for (size_t index = 1; index < trees.size (); ++index) {
            const DataTree<T>& next = AsTyped<T> (*trees[index].tree);
            std::shared_ptr<const DataTree<T>> merged;
            if (!MergeTrees (*combined, next, contract.collision, merged, error))
                return false;
            combined = std::move (merged);
        }
        result = MakeTreeValue<T> (std::move (combined));
        return true;
    });
}

std::optional<Value> ItemForCursor (const TreeValue& input, const InputCursor& cursor)
{
    if (!input.IsPresent () || !cursor.present)
        return std::nullopt;
    if (cursor.listIndex >= input.tree->ListCount ())
        return std::nullopt;

    const IDataList& list = input.tree->ListAt (cursor.listIndex);
    if (cursor.itemIndex >= list.Size ())
        return std::nullopt;
    return list.ValueAt (cursor.itemIndex);
}

bool SliceForAccess (const TreeValue& input, PortAccess access, const InputCursor& cursor, TreeValue& slice,
                     std::string& error)
{
    if (!input.IsPresent ()) {
        error = "Cannot slice an absent input";
        return false;
    }

    if (access == PortAccess::Tree) {
        slice = input; // Shared, not copied: the body reads it and does not own it.
        return true;
    }

    if (!cursor.present || cursor.listIndex >= input.tree->ListCount ()) {
        slice = EmptyTreeValue (input.itemType);
        return true;
    }

    const DataPath& path = input.tree->Paths ()[cursor.listIndex];
    const IDataList& list = input.tree->ListAt (cursor.listIndex);

    AnyTreeBuilder builder (input.itemType);
    if (access == PortAccess::List) {
        if (!builder.AddList (path, list, error))
            return false;
        slice = std::move (builder).Finish ();
        return true;
    }

    // Item access as a tree: the one item, at its own path, which is what a
    // body declared for Item access sees when it asks for a tree anyway.
    if (cursor.itemIndex >= list.Size ()) {
        builder.EnsureList (path);
        slice = std::move (builder).Finish ();
        return true;
    }

    const std::optional<Value> value = list.ValueAt (cursor.itemIndex);
    if (!value.has_value ())
        builder.AddNull (path, list.MetadataAt (cursor.itemIndex));
    else if (!builder.Add (path, *value, list.MetadataAt (cursor.itemIndex), error))
        return false;

    slice = std::move (builder).Finish ();
    return true;
}

// ---- Erased topology operations ---------------------------------------------

namespace {

// The one guard every wrapper below needs before it can dispatch on
// `input.itemType`: an absent tree has no type to dispatch on, and the ports
// this file serves never hand a body one anyway (§7.5 - a missing site arrives
// as the empty tree, not as no tree), so reaching here means a caller outside
// the lifted contract.
bool RequirePresent (const TreeValue& input, const char* operation, std::string& error)
{
    if (input.IsPresent ())
        return true;
    error = std::string ("Cannot ") + operation + " an absent tree";
    return false;
}

} // namespace

bool FlattenTreeValue (const TreeValue& input, TreeValue& result, std::string& error)
{
    if (!RequirePresent (input, "flatten", error))
        return false;
    return DispatchItemType (input.itemType, [&]<class T> () {
        result = MakeTreeValue<T> (FlattenTree (AsTyped<T> (*input.tree)));
        return true;
    });
}

bool GraftTreeValue (const TreeValue& input, TreeValue& result, std::string& error)
{
    if (!RequirePresent (input, "graft", error))
        return false;
    return DispatchItemType (input.itemType, [&]<class T> () {
        result = MakeTreeValue<T> (GraftTree (AsTyped<T> (*input.tree)));
        return true;
    });
}

bool SimplifyTreeValue (const TreeValue& input, TreeValue& result, std::string& error)
{
    if (!RequirePresent (input, "simplify", error))
        return false;
    return DispatchItemType (input.itemType, [&]<class T> () {
        // Takes the shared_ptr, not the tree, for the same reason SimplifyTree
        // itself does: that is the only signature that lets "already simple"
        // hand back the identical pointer instead of an equal copy of it.
        std::shared_ptr<const DataTree<T>> original = std::static_pointer_cast<const DataTree<T>> (input.tree);
        result = MakeTreeValue<T> (SimplifyTree (original));
        return true;
    });
}

bool ShiftTreeValuePaths (const TreeValue& input, int32_t shift, PathCollision policy, TreeValue& result,
                          std::string& error)
{
    if (!RequirePresent (input, "shift", error))
        return false;
    return DispatchItemType (input.itemType, [&]<class T> () {
        std::shared_ptr<const DataTree<T>> shifted;
        if (!ShiftTreePaths (AsTyped<T> (*input.tree), shift, policy, shifted, error))
            return false;
        result = MakeTreeValue<T> (std::move (shifted));
        return true;
    });
}

} // namespace evp::nodegraph::data
