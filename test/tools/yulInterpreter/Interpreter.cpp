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
// SPDX-License-Identifier: GPL-3.0
/**
 * Yul interpreter.
 */

#include <test/tools/yulInterpreter/Interpreter.h>

#include <test/tools/yulInterpreter/EVMInstructionInterpreter.h>
#include <test/tools/yulInterpreter/EwasmBuiltinInterpreter.h>

#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libyul/Utilities.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/backends/wasm/WasmDialect.h>

#include <liblangutil/Exceptions.h>

#include <libsolutil/FixedHash.h>

#include <boost/algorithm/cxx11/all_of.hpp>

#include <range/v3/view/reverse.hpp>

#include <ostream>
#include <variant>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::yul::test;

using solidity::util::h256;

void InterpreterState::dumpTraceAndState(ostream& _out) const
{
	_out << "Trace:" << endl;
	for (auto const& line: trace)
		_out << "  " << line << endl;
	_out << "Memory dump:\n";
	map<u256, u256> words;
	for (auto const& [offset, value]: memory)
		words[(offset / 0x20) * 0x20] |= u256(uint32_t(value)) << (256 - 8 - 8 * static_cast<size_t>(offset % 0x20));
	for (auto const& [offset, value]: words)
		if (value != 0)
			_out << "  " << std::uppercase << std::hex << std::setw(4) << offset << ": " << h256(value).hex() << endl;
	_out << "Storage dump:" << endl;
	for (auto const& slot: storage)
		if (slot.second != h256{})
			_out << "  " << slot.first.hex() << ": " << slot.second.hex() << endl;
}

void Interpreter::run(InterpreterState& _state, Dialect const& _dialect, Block const& _ast)
{
	Scope scope;
	Interpreter{_state, _dialect, scope}(_ast);
}

void Interpreter::operator()(ExpressionStatement const& _expressionStatement)
{
	evaluateMulti(_expressionStatement.expression);
}

void Interpreter::operator()(Assignment const& _assignment)
{
	solAssert(_assignment.value, "");
	vector<u256> values = evaluateMulti(*_assignment.value);
	solAssert(values.size() == _assignment.variableNames.size(), "");
	for (size_t i = 0; i < values.size(); ++i)
	{
		YulString varName = _assignment.variableNames.at(i).name;
		solAssert(m_variables.count(varName), "");
		m_variables[varName] = values.at(i);
	}
}

void Interpreter::operator()(VariableDeclaration const& _declaration)
{
	vector<u256> values(_declaration.variables.size(), 0);
	if (_declaration.value)
		values = evaluateMulti(*_declaration.value);

	solAssert(values.size() == _declaration.variables.size(), "");
	for (size_t i = 0; i < values.size(); ++i)
	{
		YulString varName = _declaration.variables.at(i).name;
		solAssert(!m_variables.count(varName), "");
		m_variables[varName] = values.at(i);
		m_scope->names.emplace(varName, nullptr);
	}
}

void Interpreter::operator()(If const& _if)
{
	solAssert(_if.condition, "");
	if (evaluate(*_if.condition) != 0)
		(*this)(_if.body);
}

void Interpreter::operator()(Switch const& _switch)
{
	solAssert(_switch.expression, "");
	u256 val = evaluate(*_switch.expression);
	solAssert(!_switch.cases.empty(), "");
	for (auto const& c: _switch.cases)
		// Default case has to be last.
		if (!c.value || evaluate(*c.value) == val)
		{
			(*this)(c.body);
			break;
		}
}

void Interpreter::operator()(FunctionDefinition const&)
{
}

void Interpreter::operator()(ForLoop const& _forLoop)
{
	solAssert(_forLoop.condition, "");

	enterScope(_forLoop.pre);
	ScopeGuard g([this]{ leaveScope(); });

	for (auto const& statement: _forLoop.pre.statements)
	{
		visit(statement);
		if (m_state.controlFlowState == ControlFlowState::Leave)
			return;
	}
	while (evaluate(*_forLoop.condition) != 0)
	{
		// Increment step for each loop iteration for loops with
		// an empty body and post blocks to prevent a deadlock.
		if (_forLoop.body.statements.size() == 0 && _forLoop.post.statements.size() == 0)
			incrementStep();

		m_state.controlFlowState = ControlFlowState::Default;
		(*this)(_forLoop.body);
		if (m_state.controlFlowState == ControlFlowState::Break || m_state.controlFlowState == ControlFlowState::Leave)
			break;

		m_state.controlFlowState = ControlFlowState::Default;
		(*this)(_forLoop.post);
		if (m_state.controlFlowState == ControlFlowState::Leave)
			break;
	}
	if (m_state.controlFlowState != ControlFlowState::Leave)
		m_state.controlFlowState = ControlFlowState::Default;
}

void Interpreter::operator()(Break const&)
{
	m_state.controlFlowState = ControlFlowState::Break;
}

void Interpreter::operator()(Continue const&)
{
	m_state.controlFlowState = ControlFlowState::Continue;
}

void Interpreter::operator()(Leave const&)
{
	m_state.controlFlowState = ControlFlowState::Leave;
}

void Interpreter::operator()(Block const& _block)
{
	enterScope(_block);
	// Register functions.
	for (auto const& statement: _block.statements)
		if (holds_alternative<FunctionDefinition>(statement))
		{
			FunctionDefinition const& funDef = std::get<FunctionDefinition>(statement);
			m_scope->names.emplace(funDef.name, &funDef);
		}

	for (auto const& statement: _block.statements)
	{
		incrementStep();
		visit(statement);
		if (m_state.controlFlowState != ControlFlowState::Default)
			break;
	}

	leaveScope();
}

u256 Interpreter::evaluate(Expression const& _expression)
{
	ExpressionEvaluator ev(m_state, m_dialect, *m_scope, m_variables);
	ev.visit(_expression);
	return ev.value();
}

vector<u256> Interpreter::evaluateMulti(Expression const& _expression)
{
	ExpressionEvaluator ev(m_state, m_dialect, *m_scope, m_variables);
	ev.visit(_expression);
	return ev.values();
}

void Interpreter::enterScope(Block const& _block)
{
	if (!m_scope->subScopes.count(&_block))
		m_scope->subScopes[&_block] = make_unique<Scope>(Scope{
			{},
			{},
			m_scope
		});
	m_scope = m_scope->subScopes[&_block].get();
}

void Interpreter::leaveScope()
{
	for (auto const& [var, funDeclaration]: m_scope->names)
		if (!funDeclaration)
			m_variables.erase(var);
	m_scope = m_scope->parent;
	yulAssert(m_scope, "");
}

void Interpreter::incrementStep()
{
	m_state.numSteps++;
	if (m_state.maxSteps > 0 && m_state.numSteps >= m_state.maxSteps)
	{
		m_state.trace.emplace_back("Interpreter execution step limit reached.");
		BOOST_THROW_EXCEPTION(StepLimitReached());
	}
}

void ExpressionEvaluator::operator()(Literal const& _literal)
{
	incrementStep();
	static YulString const trueString("true");
	static YulString const falseString("false");

	setValue(valueOfLiteral(_literal));
}

void ExpressionEvaluator::operator()(Identifier const& _identifier)
{
	solAssert(m_variables.count(_identifier.name), "");
	incrementStep();
	setValue(m_variables.at(_identifier.name));
}

void ExpressionEvaluator::operator()(FunctionCall const& _funCall)
{
	vector<optional<LiteralKind>> const* literalArguments = nullptr;
	if (BuiltinFunction const* builtin = m_dialect.builtin(_funCall.functionName.name))
		if (!builtin->literalArguments.empty())
			literalArguments = &builtin->literalArguments;
	evaluateArgs(_funCall.arguments, literalArguments);

	if (EVMDialect const* dialect = dynamic_cast<EVMDialect const*>(&m_dialect))
	{
		if (BuiltinFunctionForEVM const* fun = dialect->builtin(_funCall.functionName.name))
		{
			EVMInstructionInterpreter interpreter(m_state);
			setValue(interpreter.evalBuiltin(*fun, _funCall.arguments, values()));
			return;
		}
	}
	else if (WasmDialect const* dialect = dynamic_cast<WasmDialect const*>(&m_dialect))
		if (dialect->builtin(_funCall.functionName.name))
		{
			EwasmBuiltinInterpreter interpreter(m_state);
			setValue(interpreter.evalBuiltin(_funCall.functionName.name, _funCall.arguments, values()));
			return;
		}

	Scope* scope = &m_scope;
	for (; scope; scope = scope->parent)
		if (scope->names.count(_funCall.functionName.name))
			break;
	yulAssert(scope, "");

	FunctionDefinition const* fun = scope->names.at(_funCall.functionName.name);
	yulAssert(fun, "Function not found.");
	yulAssert(m_values.size() == fun->parameters.size(), "");
	map<YulString, u256> variables;
	for (size_t i = 0; i < fun->parameters.size(); ++i)
		variables[fun->parameters.at(i).name] = m_values.at(i);
	for (size_t i = 0; i < fun->returnVariables.size(); ++i)
		variables[fun->returnVariables.at(i).name] = 0;

	m_state.controlFlowState = ControlFlowState::Default;
	Interpreter interpreter(m_state, m_dialect, *scope, std::move(variables));
	interpreter(fun->body);
	m_state.controlFlowState = ControlFlowState::Default;

	m_values.clear();
	for (auto const& retVar: fun->returnVariables)
		m_values.emplace_back(interpreter.valueOfVariable(retVar.name));
}

u256 ExpressionEvaluator::value() const
{
	solAssert(m_values.size() == 1, "");
	return m_values.front();
}

void ExpressionEvaluator::setValue(u256 _value)
{
	m_values.clear();
	m_values.emplace_back(std::move(_value));
}

void ExpressionEvaluator::evaluateArgs(
	vector<Expression> const& _expr,
	vector<optional<LiteralKind>> const* _literalArguments
)
{
	incrementStep();
	vector<u256> values;
	size_t i = 0;
	/// Function arguments are evaluated in reverse.
	for (auto const& expr: _expr | ranges::views::reverse)
	{
		if (!_literalArguments || !_literalArguments->at(_expr.size() - i - 1))
			visit(expr);
		else
			m_values = {0};
		values.push_back(value());
		++i;
	}
	m_values = std::move(values);
	std::reverse(m_values.begin(), m_values.end());
}

void ExpressionEvaluator::incrementStep()
{
	m_nestingLevel++;
	if (m_state.maxExprNesting > 0 && m_nestingLevel > m_state.maxExprNesting)
	{
		m_state.trace.emplace_back("Maximum expression nesting level reached.");
		BOOST_THROW_EXCEPTION(ExpressionNestingLimitReached());
	}
}
