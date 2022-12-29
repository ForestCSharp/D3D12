#include "RenderGraph.h"

void RenderGraphNode::Execute(struct RenderGraph& render_graph, ComPtr<ID3D12GraphicsCommandList4> in_command_list)
{
	unordered_map<string, RenderGraphNode>& nodes = render_graph.GetNodes();
	multimap<string, RenderGraphEdge>& incoming_edges = render_graph.GetIncomingEdges();
	const string& node_name = desc.name;

	// Find Incoming Edges relevant to this node
	auto equal_range = incoming_edges.equal_range(node_name);
	for (auto& itr = equal_range.first; itr != equal_range.second; ++itr)
	{
		// If relevant to one of our inputs, use this edge to associate our input with a previously computed output resource
		const RenderGraphEdge& edge = itr->second;
		if (edge.incoming_resource.has_value() && edge.outgoing_resource.has_value())
		{
			const string& input_to_find = *edge.outgoing_resource;
			RenderGraphInput& input = inputs.at(input_to_find);
			RenderGraphNode& other_node = nodes.at(edge.incoming_node);
			RenderGraphOutput& other_node_output = other_node.outputs.at(*edge.incoming_resource);
			input.incoming_resource = &other_node_output;

			// if input and output resource states don't agree, we'll need a barrier
			const D3D12_RESOURCE_STATES prev_state = other_node_output.GetResourceState();
			const D3D12_RESOURCE_STATES new_state = input.GetResourceState();
			if (prev_state != new_state)
			{
				CmdBarrier(in_command_list, input.GetD3D12Resource(), prev_state, new_state);
			}
		}
	}

	desc.execute(*this, in_command_list);
}

void RenderGraph::AddNode(const RenderGraphNodeDesc&& in_desc)
{
	assert(in_desc.setup && in_desc.execute);

	//Create actual node
	RenderGraphNode new_node(in_desc);

	//Immediately run setup
	new_node.Setup(m_allocator.Get());

	//Insert
	nodes.insert({ in_desc.name, new_node });
}

// TODO: Cycle detection
void RenderGraph::AddEdge(const RenderGraphEdge&& in_edge)
{
	auto input_node = nodes.find(in_edge.incoming_node);
	assert(input_node != nodes.end());
	if (in_edge.incoming_resource.has_value())
	{
		assert(input_node->second.outputs.contains(in_edge.incoming_resource.value()));
	}

	auto output_node = nodes.find(in_edge.outgoing_node);
	assert(output_node != nodes.end());
	if (in_edge.outgoing_resource.has_value())
	{
		assert(output_node->second.inputs.contains(in_edge.outgoing_resource.value()));
	}

	//TODO: make sure types of 2 resources agree

	incoming_edges.insert({in_edge.outgoing_node, in_edge});
	outgoing_edges.insert({in_edge.incoming_node, in_edge});
}

vector<RenderGraphNode*> RenderGraph::RecurseNodes(const vector<string>& in_node_names, unordered_map<string, size_t> visited_nodes)
{
	assert(in_node_names.size() > 0);

	/*	
		We recursively build up our sorted nodes by pushing them onto an array, which will
		later be iterated over in reverse order to correctly evaulate dependencies before
		a given node
	*/
	vector<RenderGraphNode*> sorted_nodes;

	// Add in_node_names to sorted nodes
	for (const string& node_name : in_node_names)
	{
		/*	
			If we've already visited this node, we've now found an earlier dependency that 
			we need to satisfy. So remove it, and then re-add it on the end
		*/
		if (visited_nodes.contains(node_name))
		{
			sorted_nodes.erase(sorted_nodes.begin() + visited_nodes.at(node_name));
		}
		sorted_nodes.push_back(&nodes.find(node_name)->second);
		visited_nodes[node_name] = sorted_nodes.size() - 1;
	}

	// Build list of dependency node names we'll need to process
	vector<string> dependency_node_names;
	for (const string& node_name : in_node_names)
	{
		auto equal_range = incoming_edges.equal_range(node_name);
		for (auto& itr = equal_range.first; itr != equal_range.second; ++itr)
		{
			dependency_node_names.push_back(itr->second.incoming_node);
		}
	}

	// Base Case: No child nodes left to process
	if (dependency_node_names.size() > 0)
	{
		vector<RenderGraphNode*> child_sorted_nodes = RecurseNodes(dependency_node_names, visited_nodes);
		sorted_nodes.insert(sorted_nodes.end(), child_sorted_nodes.begin(), child_sorted_nodes.end());
	}

	return sorted_nodes;
};

void RenderGraph::Execute()
{
	// 1. Gather terminal nodes (Terminal nodes are any node that lack outgoing edges)
	vector<string> terminal_node_names;
	for (auto& [node_name,_] : nodes)
	{
		if (!outgoing_edges.contains(node_name))
		{
			terminal_node_names.push_back(node_name);
		}
	}

	// 2. Work backwards from terminal nodes to determine flow of execution (sort nodes)
	unordered_map<string, size_t> visited_nodes;
	vector<RenderGraphNode*> sorted_nodes = RecurseNodes(terminal_node_names, visited_nodes);

	// 3. Work backwards through sorted nodes + execute them
	for (auto it = sorted_nodes.rbegin(); it != sorted_nodes.rend(); ++it)
	{
		RenderGraphNode& node = **it;
		node.Execute(*this, m_command_list);
	}
}