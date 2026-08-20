#include "DeferredBindings.hpp"

#include <memory>
#include <string>
#include <vector>

namespace evp {
namespace {

struct ValueNode {
    enum class Kind { Object, List, Bool, Int, UInt, Real, String };

    Kind kind = Kind::Object;
    std::vector<GS::String> names;
    std::vector<std::unique_ptr<ValueNode>> values;
    bool boolValue = false;
    Int64 intValue = 0;
    UInt64 uintValue = 0;
    double realValue = 0.0;
    GS::UniString stringValue;

    ValueNode () = default;

    ValueNode (const ValueNode& other) :
        kind (other.kind),
        names (other.names),
        boolValue (other.boolValue),
        intValue (other.intValue),
        uintValue (other.uintValue),
        realValue (other.realValue),
        stringValue (other.stringValue)
    {
        for (const auto& value : other.values)
            values.push_back (std::make_unique<ValueNode> (*value));
    }

    ValueNode* Find (const GS::String& name)
    {
        for (std::size_t i = 0; i < names.size (); ++i) {
            if (names[i] == name)
                return values[i].get ();
        }
        return nullptr;
    }

    const ValueNode* Find (const GS::String& name) const
    {
        return const_cast<ValueNode*> (this)->Find (name);
    }

    void Set (const GS::String& name, const ValueNode& value)
    {
        for (std::size_t i = 0; i < names.size (); ++i) {
            if (names[i] == name) {
                values[i] = std::make_unique<ValueNode> (value);
                return;
            }
        }
        names.push_back (name);
        values.push_back (std::make_unique<ValueNode> (value));
    }
};

class TreeBuilder final : public GS::ObjectState::Processor {
public:
    ValueNode root;

    TreeBuilder () { stack.push_back (&root); }

    void BoolFound (const GS::String& name, bool value) override
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = ValueNode::Kind::Bool;
        node->boolValue = value;
        Add (name, std::move (node));
    }

    void IntFound (const GS::String& name, Int64 value) override
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = ValueNode::Kind::Int;
        node->intValue = value;
        Add (name, std::move (node));
    }

    void UIntFound (const GS::String& name, UInt64 value) override
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = ValueNode::Kind::UInt;
        node->uintValue = value;
        Add (name, std::move (node));
    }

    void RealFound (const GS::String& name, double value) override
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = ValueNode::Kind::Real;
        node->realValue = value;
        Add (name, std::move (node));
    }

    void StringFound (const GS::String& name, const GS::UniString& value) override
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = ValueNode::Kind::String;
        node->stringValue = value;
        Add (name, std::move (node));
    }

    bool ObjectFound (const GS::String&, const GS::ObjectState&) override { return true; }
    void ObjectEntered (const GS::String& name) override { Enter (name, ValueNode::Kind::Object); }
    void ObjectExited (const GS::String&) override { stack.pop_back (); }
    bool ListFound (const GS::String&) override { return true; }
    void ListEntered (const GS::String& name) override { Enter (name, ValueNode::Kind::List); }
    void ListExited (const GS::String&) override { stack.pop_back (); }

private:
    std::vector<ValueNode*> stack;

    ValueNode* Add (const GS::String& name, std::unique_ptr<ValueNode> node)
    {
        ValueNode* parent = stack.back ();
        ValueNode* added = node.get ();
        if (parent->kind == ValueNode::Kind::Object)
            parent->names.push_back (name);
        parent->values.push_back (std::move (node));
        return added;
    }

    void Enter (const GS::String& name, ValueNode::Kind kind)
    {
        auto node = std::make_unique<ValueNode> ();
        node->kind = kind;
        stack.push_back (Add (name, std::move (node)));
    }
};

void EmitValue (const ValueNode& node, GS::ObjectState::ContentProcessor& processor)
{
    switch (node.kind) {
        case ValueNode::Kind::Bool:   processor.BoolFound (node.boolValue); break;
        case ValueNode::Kind::Int:    processor.IntFound (node.intValue); break;
        case ValueNode::Kind::UInt:   processor.UIntFound (node.uintValue); break;
        case ValueNode::Kind::Real:   processor.RealFound (node.realValue); break;
        case ValueNode::Kind::String: processor.StringFound (node.stringValue); break;
        case ValueNode::Kind::Object:
            processor.ObjectEntered ();
            for (std::size_t i = 0; i < node.values.size (); ++i) {
                processor.FieldFound (node.names[i]);
                EmitValue (*node.values[i], processor);
            }
            processor.ObjectExited ();
            break;
        case ValueNode::Kind::List:
            processor.ListEntered ();
            for (const auto& value : node.values)
                EmitValue (*value, processor);
            processor.ListExited ();
            break;
    }
}

class TreeContent final : public GS::ObjectState::Content {
public:
    explicit TreeContent (const ValueNode& root) : root (root) {}

    void Enumerate (GS::ObjectState::ContentProcessor& processor) const override
    {
        for (std::size_t i = 0; i < root.values.size (); ++i) {
            processor.FieldFound (root.names[i]);
            EmitValue (*root.values[i], processor);
        }
    }

private:
    const ValueNode& root;
};

bool SplitPath (const GS::UniString& path, std::vector<GS::String>& parts)
{
    const std::string text (path.ToCStr ().Get ());
    std::size_t start = 0;
    while (start <= text.size ()) {
        const std::size_t dot = text.find ('.', start);
        const std::string part = text.substr (start, dot - start);
        if (part.empty ())
            return false;
        parts.emplace_back (part.c_str ());
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return !parts.empty ();
}

const ValueNode* Resolve (const ValueNode& root, const std::vector<GS::String>& path)
{
    const ValueNode* current = &root;
    for (const GS::String& part : path) {
        if (current->kind != ValueNode::Kind::Object)
            return nullptr;
        current = current->Find (part);
        if (current == nullptr)
            return nullptr;
    }
    return current;
}

bool Set (ValueNode& root, const std::vector<GS::String>& path, const ValueNode& value)
{
    ValueNode* current = &root;
    for (std::size_t i = 0; i + 1 < path.size (); ++i) {
        if (current->kind != ValueNode::Kind::Object)
            return false;
        ValueNode* child = current->Find (path[i]);
        if (child == nullptr) {
            ValueNode object;
            current->Set (path[i], object);
            child = current->Find (path[i]);
        }
        if (child->kind != ValueNode::Kind::Object)
            return false;
        current = child;
    }
    if (current->kind != ValueNode::Kind::Object)
        return false;
    current->Set (path.back (), value);
    return true;
}

const char* KindName (ValueNode::Kind kind)
{
    switch (kind) {
        case ValueNode::Kind::Object: return "object";
        case ValueNode::Kind::List:   return "list";
        case ValueNode::Kind::Bool:   return "boolean";
        case ValueNode::Kind::Int:
        case ValueNode::Kind::UInt:   return "integer";
        case ValueNode::Kind::Real:   return "number";
        case ValueNode::Kind::String: return "string";
    }
    return "unknown";
}

} // namespace

bool ApplyDeferredBinding (const GS::ObjectState& source, const GS::UniString& sourcePath,
                           GS::ObjectState& target, const GS::UniString& targetPath,
                           GS::UniString& error)
{
    std::vector<GS::String> sourceParts;
    std::vector<GS::String> targetParts;
    if (!SplitPath (sourcePath, sourceParts) || !SplitPath (targetPath, targetParts)) {
        error = "binding paths must be non-empty dot-separated object field names";
        return false;
    }

    TreeBuilder sourceBuilder;
    source.Enumerate (sourceBuilder);
    const ValueNode* value = Resolve (sourceBuilder.root, sourceParts);
    if (value == nullptr) {
        error = GS::UniString::Printf ("source path '%T' does not exist or traverses a non-object value",
                                       sourcePath.ToPrintf ());
        return false;
    }

    TreeBuilder targetBuilder;
    target.Enumerate (targetBuilder);
    if (!Set (targetBuilder.root, targetParts, *value)) {
        error = GS::UniString::Printf ("target path '%T' traverses a non-object value while binding %s",
                                       targetPath.ToPrintf (), KindName (value->kind));
        return false;
    }

    GS::ObjectState rebuilt;
    const TreeContent content (targetBuilder.root);
    if (rebuilt.Build (content) != NoError) {
        error = "could not rebuild request after applying deferred binding";
        return false;
    }
    target = std::move (rebuilt);
    return true;
}

} // namespace evp
