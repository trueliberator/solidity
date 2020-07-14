/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libyul/optimiser/StackLimitEvader.h>
#include <libyul/optimiser/CallGraphGenerator.h>
#include <libyul/optimiser/FunctionCallFinder.h>
#include <libyul/optimiser/NameDispenser.h>
#include <libyul/optimiser/StackToMemoryMover.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/AsmData.h>
#include <libyul/CompilabilityChecker.h>
#include <libyul/Dialect.h>
#include <libyul/Exceptions.h>
#include <libyul/Object.h>
#include <libyul/Utilities.h>
#include <libsolutil/Algorithms.h>
#include <libsolutil/CommonData.h>
#include <libevmasm/Exceptions.h>

using namespace std;
using namespace solidity;
using namespace solidity::yul;

namespace
{
// Walks the call graph using a Depth-First-Search assigning memory offsets to variables.
// - The leaves of the call graph will get the lowest offsets, increasing towards the root.
// - ``nextAvailableSlot`` maps a function to the next available slot that can be used by another
//   function that calls it.
// - For each function starting from the root of the call graph:
//   - Visit all children that are not already visited.
//   - Determe the maximum value ``n`` of the values of ``nextAvailableSlot`` among the children.
//   - If the function itself contains variables that need memory slots, but is contained in a cycle,
//     abort the process as failure.
//   - If not, assign each variable its slot starting starting from ``n`` (incrementing it).
//   - Assign ``nextAvailableSlot`` of the function to ``n``.
struct MemoryOffsetAllocator
{
	map<YulString, FunctionStackErrorInfo> const& functionStackErrorInfo;
	map<YulString, std::set<YulString>> const& callGraph;

	uint64_t run(YulString _function = YulString{})
	{
		if (nextAvailableSlot.count(_function))
			return nextAvailableSlot[_function];

		// Assign to zero early to guard against recursive calls.
		nextAvailableSlot[_function] = 0;

		uint64_t nextSlot = 0;
		if (callGraph.count(_function))
			for (auto child: callGraph.at(_function))
				nextSlot = std::max(run(child), nextSlot);

		if (functionStackErrorInfo.count(_function))
		{
			auto const& stackErrorInfo = functionStackErrorInfo.at(_function);
			yulAssert(!slotAllocations.count(_function), "");
			auto& assignedSlots = slotAllocations[_function];
			for (auto const& variable: stackErrorInfo.variables)
				if (variable.empty())
				{
					// TODO: Too many function arguments or return parameters.
				}
				else
					assignedSlots[variable] = nextSlot++;
		}

		return (nextAvailableSlot[_function] = nextSlot);
	}

	map<YulString, map<YulString, uint64_t>> slotAllocations{};
	map<YulString, uint64_t> nextAvailableSlot{};
};
}

void StackLimitEvader::run(OptimiserStepContext& _context, Object& _object, bool _optimizeStackAllocation)
{
	// Determine which variables need to be moved.
	map<YulString, FunctionStackErrorInfo> functionStackErrorInfo = CompilabilityChecker::run(
		_context.dialect,
		_object,
		_optimizeStackAllocation
	);
	if (functionStackErrorInfo.empty())
		return;

	run(_context, _object, functionStackErrorInfo);
}

void StackLimitEvader::run(
	OptimiserStepContext& _context,
	Object& _object,
	std::map<YulString, FunctionStackErrorInfo> const& _functionStackErrorInfo)
{
	yulAssert(_object.code, "");
	auto const* evmDialect = dynamic_cast<EVMDialect const*>(&_context.dialect);
	yulAssert(
		evmDialect && evmDialect->providesObjectAccess(),
		"StackToMemoryMover can only be run on objects using the EVMDialect with object access."
	);

	// Find the literal argument of the ``memoryinit`` call, if there is a unique such call, otherwise abort.
	Literal* memoryInitLiteral = nullptr;
	if (
		auto memoryInits = FunctionCallFinder::run(*_object.code, "memoryinit"_yulstring);
		memoryInits.size() == 1
	)
		memoryInitLiteral = std::get_if<Literal>(&memoryInits.front()->arguments.back());
	if (!memoryInitLiteral)
		return;
	u256 reservedMemory = valueOfLiteral(*memoryInitLiteral);

	map<YulString, set<YulString>> callGraph = CallGraphGenerator::callGraph(*_object.code).functionCalls;

	// Collect all names of functions contained in cycles in the callgraph.
	// TODO: this algorithm is suboptimal and can be improved. It also overlaps with Semantics.cpp.
	std::set<YulString> containedInCycle;
	auto findCycles = [
		&,
		visited = std::map<YulString, uint64_t>{},
		currentPath = std::vector<YulString>{}
	](YulString _node, auto& _recurse) mutable -> void
	{
		if (auto it = std::find(currentPath.begin(), currentPath.end(), _node); it != currentPath.end())
			containedInCycle.insert(it, currentPath.end());
		else
		{
			visited[_node] = currentPath.size();
			currentPath.emplace_back(_node);
			for (auto const& child: callGraph[_node])
				_recurse(child, _recurse);
			currentPath.pop_back();
		}
	};
	findCycles(YulString{}, findCycles);

	for (YulString function: containedInCycle)
		if (_functionStackErrorInfo.count(function))
			return;

	MemoryOffsetAllocator memoryOffsetAllocator{_functionStackErrorInfo, callGraph};
	uint64_t requiredSlots = memoryOffsetAllocator.run();

	StackToMemoryMover{_context, reservedMemory, memoryOffsetAllocator.slotAllocations}(*_object.code);
	memoryInitLiteral->value = YulString{util::toCompactHexWithPrefix(reservedMemory + 32 * requiredSlots)};
}