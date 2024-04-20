#include "graph.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace gigagrad
{

static dim_t FixDim(dim_t dim, dim_t mod)
{
    auto fixed_dim = ((dim % mod) + mod) % mod;
    return fixed_dim;
}

static Shape GetBroadcastedShape(const Shape &x, const Shape &y)
{
    // Ensure x.size() >= y.size()
    Shape larger = x.size() > y.size() ? x : y;
    const Shape &smaller = x.size() > y.size() ? y : x;

    for(ssize_t i = 0; i < std::ssize(smaller); i++)
    {
        // Store the proper dimension in dim_x
        auto &dim_x = larger[larger.size() - i - 1];
        const auto &dim_y = smaller[smaller.size() - i - 1];
        if(dim_x == 1 && dim_y != 1)
            dim_x = dim_y;
        else if(dim_x != 1 && dim_y == 1)
            continue;
        else if(dim_x == dim_y)
            continue;
        else
            throw std::domain_error("Cannot broadcast incompatible shapes");
    }
    return larger;
}

static Shape ComputeStrides(Shape shape)
{
    dim_t cur = 1;
    for(ssize_t i = std::ssize(shape) - 1; i >= 0; i--)
    {
        auto tmp = shape[i];
        shape[i] = tmp == 1 ? 0 : cur;
        cur *= tmp;
    }
    return shape;
}

static Shape ComputeReducedShape(const ReduceOp &op)
{
    Shape shape = op.x.shape();
    if(op.dims.empty())
    {
        if(op.keepdim)
        {
            std::fill(shape.begin(), shape.end(), 1);
            return shape;
        }
        return {};
    }

    if(op.dims.size() > shape.size())
        throw std::domain_error("Specified more dims to reduce on than there are dimensions in tensor");
    
    for(auto dim : op.dims)
        shape[dim] = -1; // Mark it as -1 for now. We'll either remove it or change it to 1 later

    if(!op.keepdim)
    {
        shape.erase(std::remove(shape.begin(), shape.end(), -1), shape.end());
    }
    else
    {
        std::replace(shape.begin(), shape.end(), -1, 1);
    }
    return shape;
}

static GraphNodeHandle WrapInUnary(GraphNodeHandle x, UnaryOpType type)
{
    Graph *graph = x.graph;
    return graph->AddNode(UnaryOp{type, x});
}

static GraphNodeHandle WrapInReduction(GraphNodeHandle x, ReduceOpType type, Dims dims, bool keepdim)
{
    Graph *graph = x.graph;
    for(dim_t &d : dims)
        d = FixDim(d, static_cast<dim_t>(x.shape().size()));
    std::sort(dims.begin(), dims.end());
    return graph->AddNode(ReduceOp{type, x, std::move(dims), keepdim});
}

GraphNodeHandle GraphNodeHandle::sum(bool keepdim) const
{
    Dims dims(this->shape().size());
    std::iota(dims.begin(), dims.end(), 0);
    return this->sum(std::move(dims), keepdim);
}

GraphNodeHandle GraphNodeHandle::sum(dim_t dim, bool keepdim) const
{
    return this->sum(Dims{dim}, keepdim);
}

GraphNodeHandle GraphNodeHandle::sum(Dims dims, bool keepdim) const
{
    return WrapInReduction(*this, ReduceOpType::SUM, std::move(dims), keepdim);
}

GraphNodeHandle GraphNodeHandle::max(bool keepdim) const
{
    Dims dims(this->shape().size());
    std::iota(dims.begin(), dims.end(), 0);
    return this->max(std::move(dims), keepdim);
}

GraphNodeHandle GraphNodeHandle::max(dim_t dim, bool keepdim) const
{
    return this->max(Dims{dim}, keepdim);
}

GraphNodeHandle GraphNodeHandle::max(Dims dims, bool keepdim) const
{
    return WrapInReduction(*this, ReduceOpType::MAX, std::move(dims), keepdim);
}

GraphNodeHandle GraphNodeHandle::reshape(Shape new_shape) const
{
    Graph *graph = this->graph;
    Shape input_shape = this->shape();
    auto num_elements = std::accumulate(input_shape.begin(), input_shape.end(), dim_t{1}, std::multiplies{});
    auto num_implicit_dims = std::count(new_shape.begin(), new_shape.end(), -1);
    if(num_implicit_dims == 0)
    {
        auto new_num_elements = std::accumulate(new_shape.begin(), new_shape.end(), dim_t{1}, std::multiplies{});
        if(new_num_elements != num_elements)
            throw std::domain_error("Reshape number of elements doesn't match that of input tensor");
        Shape strides = ComputeStrides(new_shape);
        return graph->AddNode(ViewOp{*this}, std::move(new_shape), std::move(strides));
    }

    if(num_implicit_dims > 1)
        throw std::domain_error("Reshape can have at most one implicit dimension");

    auto num_elems_not_including_implicit_dim = std::accumulate(
        new_shape.begin(),
        new_shape.end(),
        dim_t{1},
        [](auto x, auto y)
        {
            if(y == -1)
                return x;
            return x * y;
        });
    auto remaining_dim = num_elements / num_elems_not_including_implicit_dim;
    for(auto &x : new_shape)
        if(x == -1)
            x = remaining_dim;
    
    Shape strides = ComputeStrides(new_shape);
    return graph->AddNode(ViewOp{*this}, std::move(new_shape), std::move(strides));
}

GraphNodeHandle GraphNodeHandle::reshape(dim_t length) const
{
    return this->reshape(Shape{length});
}

GraphNodeHandle GraphNodeHandle::permute(Dims dims) const
{
    Graph *graph = this->graph;
    Shape shape = this->shape();
    if(dims.size() != shape.size())
        throw std::domain_error("Permute not given proper number of dimensions");
    std::vector<bool> uniqueness(shape.size(), false);
    Shape new_shape(shape.size());
    for(size_t i = 0; i < shape.size(); i++)
    {
        // If dim is negative, we need to fix it to be between 0 and shape.size()
        auto dim = dims[i];
        auto fixed_dim = FixDim(dim, static_cast<dim_t>(shape.size()));
        if(uniqueness[fixed_dim])
            throw std::domain_error("Found repeated dim in permute");
        uniqueness[fixed_dim] = true;
        new_shape[fixed_dim] = shape[i];
    }
    Shape strides = ComputeStrides(new_shape);
    return graph->AddNode(ViewOp{*this}, std::move(new_shape), std::move(strides));
}

GraphNodeHandle GraphNodeHandle::transpose() const
{
    Shape shape = this->shape();
    Dims dims(shape.size());
    std::iota(std::rbegin(dims), std::rend(dims), 0);
    return this->permute(std::move(dims));
}

// Matmul is a little tricky. We abuse the broadcasting semantics as follows:
// If we have matrices X, Y of shape AxB and BxC, then we reshape X into a
// AxBx1 tensor, and reshape Y into a 1xBxC matrix. Broadcasting then turns this
// into a cube of multiplications, and then we reduce along the middle axis
// and cut out the middle axis (since it has dim 1 anyway)
GraphNodeHandle GraphNodeHandle::matmul(GraphNodeHandle y) const
{
    Shape x_shape = this->shape();
    Shape y_shape = y.shape();

    // Special case for 1-D vectors by padding them up to 2D
    if(x_shape.size() == 1)
        x_shape.insert(x_shape.begin(), 1);
    if(y_shape.size() == 1)
        y_shape.push_back(1);

    if(x_shape.size() < 2 || y_shape.size() < 2)
        throw std::domain_error("Shapes must be at least of size 2 for matmul");

    x_shape.push_back(1);
    y_shape.insert(y_shape.end() - 2, 1);
    if(*(x_shape.end() - 2) != *(y_shape.end() - 2))
        throw std::domain_error("Incompatible shapes in matmul");

    GraphNodeHandle x_reshaped = this->reshape(std::move(x_shape));
    GraphNodeHandle y_reshaped = y.reshape(std::move(y_shape));
    GraphNodeHandle elementwise_mul = x_reshaped * y_reshaped;
    return elementwise_mul.sum(-2, false /* keepdim */); // Sum along the middle axis
}

GraphNodeHandle exp(GraphNodeHandle x)
{
    return WrapInUnary(x, UnaryOpType::EXP);
}

GraphNodeHandle log(GraphNodeHandle x)
{
    return WrapInUnary(x, UnaryOpType::LOG);
}

GraphNodeHandle sin(GraphNodeHandle x)
{
    return WrapInUnary(x, UnaryOpType::SIN);
}

GraphNodeHandle cos(GraphNodeHandle x)
{
    return WrapInUnary((x + 3.14159265f/2.0f), UnaryOpType::SIN);
}

GraphNodeHandle sigmoid(GraphNodeHandle x)
{
    return 1.0 / (1.0 + exp(-x));
}

GraphNodeHandle operator-(GraphNodeHandle x)
{
    return -1 * x;
}

GraphNodeHandle operator+(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::ADD, x, y});
}

GraphNodeHandle operator+(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return xnode + y;
}

GraphNodeHandle operator+(GraphNodeHandle x, float y)
{
    return y + x;
}

GraphNodeHandle operator-(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::SUB, x, y});
}

GraphNodeHandle operator-(float x, GraphNodeHandle y)
{
    return (-x) + y;
}

GraphNodeHandle operator-(GraphNodeHandle x, float y)
{
    return x + (-y);
}

GraphNodeHandle operator*(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::MUL, x, y});
}

GraphNodeHandle operator*(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return xnode * y;
}

GraphNodeHandle operator*(GraphNodeHandle x, float y)
{
    return y * x;
}

GraphNodeHandle operator/(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::DIV, x, y});
}

GraphNodeHandle operator/(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return xnode / y;
}

GraphNodeHandle operator/(GraphNodeHandle x, float y)
{
    Graph *graph = x.graph;
    GraphNodeHandle ynode = graph->AddNode(Immediate{y});
    return x / ynode;
}

GraphNodeHandle operator^(GraphNodeHandle x, float y)
{
    Graph *graph = x.graph;
    GraphNodeHandle ynode = graph->AddNode(Immediate{y});
    return graph->AddNode(BinaryOp{BinaryOpType::POW, x, ynode});
}

GraphNodeHandle operator==(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::CMP, x, y});
}

GraphNodeHandle operator==(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return xnode == y;
}

GraphNodeHandle operator==(const GraphNodeHandle x, float y)
{
    return y == x;
}

GraphNodeHandle operator<(const GraphNodeHandle x, float y)
{
    return y > x;
}

GraphNodeHandle operator<(float x, const GraphNodeHandle y)
{
    return y > x;
}

GraphNodeHandle operator<(GraphNodeHandle x, GraphNodeHandle y)
{
    return y > x;
}

GraphNodeHandle operator<=(GraphNodeHandle x, float y)
{
    return max(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle operator<=(float x, const GraphNodeHandle y)
{
    return max(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle operator<=(GraphNodeHandle x, GraphNodeHandle y)
{
    return max(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle operator>(GraphNodeHandle x, float y)
{
    return max(x, y) == x;
}

GraphNodeHandle operator>(float x, GraphNodeHandle y)
{
    return max(x, y) == x;
}

GraphNodeHandle operator>(GraphNodeHandle x, GraphNodeHandle y)
{
    return max(x, y) == x;
}

GraphNodeHandle operator>=(GraphNodeHandle x, float y)
{
    return min(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle operator>=(float x, GraphNodeHandle y)
{
    return min(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle operator>=(GraphNodeHandle x, GraphNodeHandle y)
{
    return min(x - y, 0.0f) == 0.0f;
}

GraphNodeHandle max(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::MAX, x, y});
}

GraphNodeHandle max(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return max(xnode, y);
}

GraphNodeHandle max(GraphNodeHandle x, float y)
{
    return max(y, x);
}

GraphNodeHandle sum(GraphNodeHandle x, bool keepdim)
{
    return x.sum(keepdim);
}

GraphNodeHandle min(GraphNodeHandle x, GraphNodeHandle y)
{
    return -max(-x, -y);
}

GraphNodeHandle min(float x, GraphNodeHandle y)
{
    return -max(-x, -y);
}

GraphNodeHandle min(GraphNodeHandle x, float y)
{
    return -max(-x, -y);
}

GraphNodeHandle pow(GraphNodeHandle x, float y)
{
    Graph *graph = x.graph;
    GraphNodeHandle ynode = graph->AddNode(Immediate{y});
    return graph->AddNode(BinaryOp{BinaryOpType::POW, x, ynode});
}

GraphNodeHandle pow(float x, GraphNodeHandle y)
{
    Graph *graph = y.graph;
    GraphNodeHandle xnode = graph->AddNode(Immediate{x});
    return graph->AddNode(BinaryOp{BinaryOpType::POW, xnode, y});
}

GraphNodeHandle pow(GraphNodeHandle x, GraphNodeHandle y)
{
    Graph *graph = x.graph;
    return graph->AddNode(BinaryOp{BinaryOpType::POW, x, y});
}

GraphNodeHandle sum(GraphNodeHandle x, dim_t axis, bool keepdim)
{
    return x.sum(axis, keepdim);
}

GraphNodeHandle sum(GraphNodeHandle x, Dims dims, bool keepdim)
{
    return x.sum(std::move(dims), keepdim);
}

GraphNodeHandle max(GraphNodeHandle x, bool keepdim)
{
    return x.max(keepdim);
}

GraphNodeHandle max(GraphNodeHandle x, dim_t axis, bool keepdim)
{
    return x.max(axis, keepdim);
}

GraphNodeHandle max(GraphNodeHandle x, Dims dims, bool keepdim)
{
    return x.max(std::move(dims), keepdim);
}

GraphNodeHandle min(GraphNodeHandle x, bool keepdim)
{
    return -max(-x, keepdim);
}

GraphNodeHandle min(GraphNodeHandle x, dim_t axis, bool keepdim)
{
    return -max(-x, axis, keepdim);
}

GraphNodeHandle min(GraphNodeHandle x, Dims dims, bool keepdim)
{
    return -max(-x, std::move(dims), keepdim);
}

GraphNodeHandle reshape(GraphNodeHandle x, Shape shape)
{
    return x.reshape(std::move(shape));
}

GraphNodeHandle reshape(GraphNodeHandle x, dim_t length)
{
    return x.reshape(length);
}

GraphNodeHandle permute(GraphNodeHandle x, Dims permutation)
{
    return x.permute(std::move(permutation));
}

GraphNodeHandle transpose(GraphNodeHandle x)
{
    return x.transpose();
}

GraphNodeHandle operator%(GraphNodeHandle x, GraphNodeHandle y)
{
    return x.matmul(y);
}

GraphNodeHandle matmul(GraphNodeHandle x, GraphNodeHandle y)
{
    return x.matmul(y);
}

GraphNodeHandle Graph::Immediate(float imm)
{
    return this->AddNode(gigagrad::Immediate{imm});
}

GraphNodeHandle Graph::AddInput(Shape shape)
{
    this->inputs.push_back(this->nodes.size());
    Shape strides = ComputeStrides(shape);
    GraphNodeHandle result = this->AddNode(Tensor{}, std::move(shape), std::move(strides));
    return result;
}

GraphNodeHandle Graph::AddInput(dim_t dim)
{
    return this->AddInput(Shape{dim});
}

GraphNodeHandle Graph::AddNode(Tensor tensor, Shape shape, Shape strides)
{
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(tensor) },
            .shape = std::move(shape),
            .strides = std::move(strides),
        });
}

GraphNodeHandle Graph::AddNode(struct Immediate imm)
{
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(imm) },
            .shape = {},
            .strides = {},
        });
}

GraphNodeHandle Graph::AddNode(UnaryOp op)
{
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(op) },
            .shape = op.x.shape(),
            .strides = op.x.strides(),
        });
}

GraphNodeHandle Graph::AddNode(BinaryOp op)
{
    Shape shape = GetBroadcastedShape(op.x.shape(), op.y.shape());
    Shape strides = ComputeStrides(shape);
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(op) },
            .shape = std::move(shape),
            .strides = std::move(strides), 
        });
}

GraphNodeHandle Graph::AddNode(ReduceOp op)
{
    Shape shape = ComputeReducedShape(op);
    Shape strides = ComputeStrides(shape);
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(op) },
            .shape = std::move(shape),
            .strides = std::move(strides),
        });
}

GraphNodeHandle Graph::AddNode(ViewOp op, Shape shape, Shape strides)
{
    return this->AddNode(
        GraphNode
        {
            .u = { std::move(op) },
            .shape = std::move(shape),
            .strides = std::move(strides),
        });
}

GraphNodeHandle Graph::AddNode(GraphNode node)
{
    GraphNodeHandle result = { this, this->nodes.size() };
    this->nodes.emplace_back(std::move(node));
    return result;
}

const Shape &GraphNodeHandle::shape() const
{
    const GraphNode &node = this->GetNode();
    return node.shape;
}

const Shape &GraphNodeHandle::strides() const
{
    const GraphNode &node = this->GetNode();
    return node.strides;
}

GraphNode &GraphNodeHandle::GetNode()
{
    return graph->nodes[node_idx];
}

const GraphNode &GraphNodeHandle::GetNode() const
{
    return graph->nodes[node_idx];
}

float *&GraphNodeHandle::data()
{
    GraphNode &node = this->GetNode();
    if(node.u.k.kind != GraphNode::Kind::Tensor)
        throw std::logic_error("Cannot call data() on non-Tensor node");
    return GetNode().u.t.tensor.data;
}

GraphNodeHandle nn::Module::AddInput(Shape shape)
{
    return this->graph.AddInput(std::move(shape));
}

GraphNodeHandle nn::Module::AddInput(dim_t dim)
{
    return this->graph.AddInput(dim);
}

GraphNodeHandle nn::Module::AddWeight(Shape shape)
{
    this->weights.push_back(this->graph.inputs.size());
    return this->graph.AddInput(std::move(shape));
}

GraphNodeHandle nn::Module::AddWeight(dim_t dim)
{
    this->weights.push_back(this->graph.inputs.size());
    return this->graph.AddInput(dim);
}

GraphNode::U::U(enum Kind kind) : k({ kind })
{
    switch(kind)
    {
    case Kind::Tensor:
        new (&this->t.tensor) Tensor;
        break;
    case Kind::Immediate:
        new (&this->i.immediate) Immediate;
        break;
    case Kind::UnaryOp:
        new (&this->u.unary_op) UnaryOp;
        break;
    case Kind::BinaryOp:
        new (&this->b.binary_op) BinaryOp;
        break;
    case Kind::ReduceOp:
        new (&this->r.reduce_op) ReduceOp;
        break;
    case Kind::ViewOp:
        new (&this->v.view_op) ViewOp;
        break;
    default:
        throw std::logic_error("Invalid node type!");
    }
}

GraphNode::U::U(const U &that) : U(that.k.kind)
{
    switch(that.k.kind)
    {
    case Kind::Tensor:
        this->t.tensor = that.t.tensor;
        break;
    case Kind::Immediate:
        this->i.immediate = that.i.immediate;
        break;
    case Kind::UnaryOp:
        this->u.unary_op = that.u.unary_op;
        break;
    case Kind::BinaryOp:
        this->b.binary_op = that.b.binary_op;
        break;
    case Kind::ReduceOp:
        this->r.reduce_op = that.r.reduce_op;
        break;
    case Kind::ViewOp:
        this->v.view_op = that.v.view_op;
        break;
    default:
        throw std::logic_error("Invalid node type! This is a bug");
    }
}

GraphNode::U::U(U &&that) : U(that.k.kind)
{
    switch(that.k.kind)
    {
    case Kind::Tensor:
        this->t.tensor = std::move(that.t.tensor);
        break;
    case Kind::Immediate:
        this->i.immediate = std::move(that.i.immediate);
        break;
    case Kind::UnaryOp:
        this->u.unary_op = std::move(that.u.unary_op);
        break;
    case Kind::BinaryOp:
        this->b.binary_op = std::move(that.b.binary_op);
        break;
    case Kind::ReduceOp:
        this->r.reduce_op = std::move(that.r.reduce_op);
        break;
    case Kind::ViewOp:
        this->v.view_op = std::move(that.v.view_op);
        break;
    default:
        throw std::logic_error("Invalid node type! This is a bug");
    }
}

GraphNode::U &GraphNode::U::operator=(const U &that)
{
    switch(that.k.kind)
    {
    case Kind::Tensor:
        this->t.tensor = that.t.tensor;
        break;
    case Kind::Immediate:
        this->i.immediate = that.i.immediate;
        break;
    case Kind::UnaryOp:
        this->u.unary_op = that.u.unary_op;
        break;
    case Kind::BinaryOp:
        this->b.binary_op = that.b.binary_op;
        break;
    case Kind::ReduceOp:
        this->r.reduce_op = that.r.reduce_op;
        break;
    case Kind::ViewOp:
        this->v.view_op = that.v.view_op;
        break;
    default:
        throw std::logic_error("Invalid node type! This is a bug");
    }
    return *this;
}

GraphNode::U &GraphNode::U::operator=(U &&that)
{
    switch(that.k.kind)
    {
    case Kind::Tensor:
        this->t.tensor = std::move(that.t.tensor);
        break;
    case Kind::Immediate:
        this->i.immediate = std::move(that.i.immediate);
        break;
    case Kind::UnaryOp:
        this->u.unary_op = std::move(that.u.unary_op);
        break;
    case Kind::BinaryOp:
        this->b.binary_op = std::move(that.b.binary_op);
        break;
    case Kind::ReduceOp:
        this->r.reduce_op = std::move(that.r.reduce_op);
        break;
    case Kind::ViewOp:
        this->v.view_op = std::move(that.v.view_op);
        break;
    default:
        throw std::logic_error("Invalid node type! This is a bug");
    }
    return *this;
}

GraphNode::U::~U()
{
    switch(this->k.kind)
    {
    case Kind::Tensor:
        this->t.tensor.~Tensor();
        break;
    case Kind::Immediate:
        this->i.immediate.~Immediate();
        break;
    case Kind::UnaryOp:
        this->u.unary_op.~UnaryOp();
        break;
    case Kind::BinaryOp:
        this->b.binary_op.~BinaryOp();
        break;
    case Kind::ReduceOp:
        this->r.reduce_op.~ReduceOp();
        break;
    case Kind::ViewOp:
        this->v.view_op.~ViewOp();
        break;
    default:
        break;
    }
}

}
