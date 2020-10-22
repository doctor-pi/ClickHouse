#include <Interpreters/Set.h>
#include <Common/ProfileEvents.h>
#include <Common/SipHash.h>
#include <Interpreters/ArrayJoinAction.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/ExpressionJIT.h>
#include <Interpreters/TableJoin.h>
#include <Interpreters/Context.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnArray.h>
#include <Common/typeid_cast.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <Functions/IFunction.h>
#include <IO/Operators.h>
#include <optional>
#include <Columns/ColumnSet.h>
#include <queue>
#include <stack>

#if !defined(ARCADIA_BUILD)
#    include "config_core.h"
#endif


namespace ProfileEvents
{
    extern const Event FunctionExecute;
    extern const Event CompiledFunctionExecute;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int DUPLICATE_COLUMN;
    extern const int UNKNOWN_IDENTIFIER;
    extern const int NOT_FOUND_COLUMN_IN_BLOCK;
    extern const int TOO_MANY_TEMPORARY_COLUMNS;
    extern const int TOO_MANY_TEMPORARY_NON_CONST_COLUMNS;
    extern const int TYPE_MISMATCH;
}

/// Read comment near usage
static constexpr auto DUMMY_COLUMN_NAME = "_dummy";


Names ExpressionAction::getNeededColumns() const
{
    Names res = argument_names;

    if (table_join)
        res.insert(res.end(), table_join->keyNamesLeft().begin(), table_join->keyNamesLeft().end());

    for (const auto & column : projection)
        res.push_back(column.first);

    if (!source_name.empty())
        res.push_back(source_name);

    return res;
}


ExpressionAction ExpressionAction::applyFunction(
    const FunctionOverloadResolverPtr & function_,
    const std::vector<std::string> & argument_names_,
    std::string result_name_)
{
    if (result_name_.empty())
    {
        result_name_ = function_->getName() + "(";
        for (size_t i = 0 ; i < argument_names_.size(); ++i)
        {
            if (i)
                result_name_ += ", ";
            result_name_ += argument_names_[i];
        }
        result_name_ += ")";
    }

    ExpressionAction a;
    a.type = APPLY_FUNCTION;
    a.result_name = result_name_;
    a.function_builder = function_;
    a.argument_names = argument_names_;
    return a;
}

ExpressionAction ExpressionAction::addColumn(
    const ColumnWithTypeAndName & added_column_)
{
    ExpressionAction a;
    a.type = ADD_COLUMN;
    a.result_name = added_column_.name;
    a.result_type = added_column_.type;
    a.added_column = added_column_.column;
    return a;
}

ExpressionAction ExpressionAction::removeColumn(const std::string & removed_name)
{
    ExpressionAction a;
    a.type = REMOVE_COLUMN;
    a.source_name = removed_name;
    return a;
}

ExpressionAction ExpressionAction::copyColumn(const std::string & from_name, const std::string & to_name, bool can_replace)
{
    ExpressionAction a;
    a.type = COPY_COLUMN;
    a.source_name = from_name;
    a.result_name = to_name;
    a.can_replace = can_replace;
    return a;
}

ExpressionAction ExpressionAction::project(const NamesWithAliases & projected_columns_)
{
    ExpressionAction a;
    a.type = PROJECT;
    a.projection = projected_columns_;
    return a;
}

ExpressionAction ExpressionAction::project(const Names & projected_columns_)
{
    ExpressionAction a;
    a.type = PROJECT;
    a.projection.resize(projected_columns_.size());
    for (size_t i = 0; i < projected_columns_.size(); ++i)
        a.projection[i] = NameWithAlias(projected_columns_[i], "");
    return a;
}

ExpressionAction ExpressionAction::addAliases(const NamesWithAliases & aliased_columns_)
{
    ExpressionAction a;
    a.type = ADD_ALIASES;
    a.projection = aliased_columns_;
    return a;
}

ExpressionAction ExpressionAction::arrayJoin(std::string source_name, std::string result_name)
{
    if (source_name == result_name)
        throw Exception("ARRAY JOIN action should have different source and result names", ErrorCodes::LOGICAL_ERROR);

    ExpressionAction a;
    a.type = ARRAY_JOIN;
    a.source_name = std::move(source_name);
    a.result_name = std::move(result_name);
    return a;
}


void ExpressionAction::prepare(Block & sample_block, const Settings & settings, NameSet & names_not_for_constant_folding)
{
    // std::cerr << "preparing: " << toString() << std::endl;

    /** Constant expressions should be evaluated, and put the result in sample_block.
      */

    switch (type)
    {
        case APPLY_FUNCTION:
        {
            if (sample_block.has(result_name))
                throw Exception("Column '" + result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);

            bool all_const = true;
            bool all_suitable_for_constant_folding = true;

            ColumnsWithTypeAndName arguments(argument_names.size());
            for (size_t i = 0; i < argument_names.size(); ++i)
            {
                arguments[i] = sample_block.getByName(argument_names[i]);
                ColumnPtr col = arguments[i].column;
                if (!col || !isColumnConst(*col))
                    all_const = false;

                if (names_not_for_constant_folding.count(argument_names[i]))
                    all_suitable_for_constant_folding = false;
            }

            size_t result_position = sample_block.columns();
            sample_block.insert({nullptr, result_type, result_name});
            if (!function)
                function = function_base->prepare(arguments);
            function->createLowCardinalityResultCache(settings.max_threads);

            bool compile_expressions = false;
#if USE_EMBEDDED_COMPILER
            compile_expressions = settings.compile_expressions;
#endif
            /// If all arguments are constants, and function is suitable to be executed in 'prepare' stage - execute function.
            /// But if we compile expressions compiled version of this function maybe placed in cache,
            /// so we don't want to unfold non deterministic functions
            if (all_const && function_base->isSuitableForConstantFolding() && (!compile_expressions || function_base->isDeterministic()))
            {
                if (added_column)
                    sample_block.getByPosition(result_position).column = added_column;
                else
                    sample_block.getByPosition(result_position).column = function->execute(arguments, result_type, sample_block.rows(), true);

                /// If the result is not a constant, just in case, we will consider the result as unknown.
                ColumnWithTypeAndName & col = sample_block.safeGetByPosition(result_position);
                if (!isColumnConst(*col.column))
                {
                    col.column = nullptr;
                }
                else
                {
                    /// All constant (literal) columns in block are added with size 1.
                    /// But if there was no columns in block before executing a function, the result has size 0.
                    /// Change the size to 1.

                    if (col.column->empty())
                        col.column = col.column->cloneResized(1);

                    if (!all_suitable_for_constant_folding)
                        names_not_for_constant_folding.insert(result_name);
                }
            }

            /// Some functions like ignore() or getTypeName() always return constant result even if arguments are not constant.
            /// We can't do constant folding, but can specify in sample block that function result is constant to avoid
            /// unnecessary materialization.
            auto & res = sample_block.getByPosition(result_position);
            if (!res.column && function_base->isSuitableForConstantFolding())
            {
                if (auto col = function_base->getResultIfAlwaysReturnsConstantAndHasArguments(arguments))
                {
                    res.column = std::move(col);
                    names_not_for_constant_folding.insert(result_name);
                }
            }

            break;
        }

        case ARRAY_JOIN:
        {
            ColumnWithTypeAndName current = sample_block.getByName(source_name);
            sample_block.erase(source_name);

            const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(&*current.type);
            if (!array_type)
                throw Exception("ARRAY JOIN requires array argument", ErrorCodes::TYPE_MISMATCH);

            current.name = result_name;
            current.type = array_type->getNestedType();
            current.column = nullptr; /// Result is never const
            sample_block.insert(std::move(current));

            break;
        }

        case PROJECT:
        {
            Block new_block;

            for (const auto & elem : projection)
            {
                const std::string & name = elem.first;
                const std::string & alias = elem.second;
                ColumnWithTypeAndName column = sample_block.getByName(name);
                if (!alias.empty())
                    column.name = alias;
                new_block.insert(std::move(column));
            }

            sample_block.swap(new_block);
            break;
        }

        case ADD_ALIASES:
        {
            for (const auto & elem : projection)
            {
                const std::string & name = elem.first;
                const std::string & alias = elem.second;
                const ColumnWithTypeAndName & column = sample_block.getByName(name);
                if (!alias.empty() && !sample_block.has(alias))
                    sample_block.insert({column.column, column.type, alias});
            }
            break;
        }

        case REMOVE_COLUMN:
        {
            sample_block.erase(source_name);
            break;
        }

        case ADD_COLUMN:
        {
            if (sample_block.has(result_name))
                throw Exception("Column '" + result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);

            sample_block.insert(ColumnWithTypeAndName(added_column, result_type, result_name));
            break;
        }

        case COPY_COLUMN:
        {
            const auto & source = sample_block.getByName(source_name);
            result_type = source.type;

            if (sample_block.has(result_name))
            {
                if (can_replace)
                {
                    auto & result = sample_block.getByName(result_name);
                    result.type = result_type;
                    result.column = source.column;
                }
                else
                    throw Exception("Column '" + result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);
            }
            else
                sample_block.insert(ColumnWithTypeAndName(source.column, result_type, result_name));

            break;
        }
    }
}

void ExpressionAction::execute(Block & block, bool dry_run) const
{
    size_t input_rows_count = block.rows();

    if (type == REMOVE_COLUMN || type == COPY_COLUMN)
        if (!block.has(source_name))
            throw Exception("Not found column '" + source_name + "'. There are columns: " + block.dumpNames(), ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);

    if (type == ADD_COLUMN || (type == COPY_COLUMN && !can_replace) || type == APPLY_FUNCTION)
        if (block.has(result_name))
            throw Exception("Column '" + result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);

    switch (type)
    {
        case APPLY_FUNCTION:
        {
            ColumnsWithTypeAndName arguments(argument_names.size());
            for (size_t i = 0; i < argument_names.size(); ++i)
                arguments[i] = block.getByName(argument_names[i]);

            size_t num_columns_without_result = block.columns();
            block.insert({ nullptr, result_type, result_name});

            ProfileEvents::increment(ProfileEvents::FunctionExecute);
            if (is_function_compiled)
                ProfileEvents::increment(ProfileEvents::CompiledFunctionExecute);
            block.getByPosition(num_columns_without_result).column = function->execute(arguments, result_type, input_rows_count, dry_run);

            break;
        }

        case ARRAY_JOIN:
        {
            auto source = block.getByName(source_name);
            block.erase(source_name);
            source.column = source.column->convertToFullColumnIfConst();

            const ColumnArray * array = typeid_cast<const ColumnArray *>(source.column.get());
            if (!array)
                throw Exception("ARRAY JOIN of not array: " + source_name, ErrorCodes::TYPE_MISMATCH);

            for (auto & column : block)
                column.column = column.column->replicate(array->getOffsets());

            source.column = array->getDataPtr();
            source.type = assert_cast<const DataTypeArray &>(*source.type).getNestedType();
            source.name = result_name;

            block.insert(std::move(source));

            break;
        }

        case PROJECT:
        {
            Block new_block;

            for (const auto & elem : projection)
            {
                const std::string & name = elem.first;
                const std::string & alias = elem.second;
                ColumnWithTypeAndName column = block.getByName(name);
                if (!alias.empty())
                    column.name = alias;
                new_block.insert(std::move(column));
            }

            block.swap(new_block);

            break;
        }

        case ADD_ALIASES:
        {
            for (const auto & elem : projection)
            {
                const std::string & name = elem.first;
                const std::string & alias = elem.second;
                const ColumnWithTypeAndName & column = block.getByName(name);
                if (!alias.empty() && !block.has(alias))
                    block.insert({column.column, column.type, alias});
            }
            break;
        }

        case REMOVE_COLUMN:
            block.erase(source_name);
            break;

        case ADD_COLUMN:
            block.insert({ added_column->cloneResized(input_rows_count), result_type, result_name });
            break;

        case COPY_COLUMN:
            if (can_replace && block.has(result_name))
            {
                auto & result = block.getByName(result_name);
                const auto & source = block.getByName(source_name);
                result.type = source.type;
                result.column = source.column;
            }
            else
            {
                const auto & source_column = block.getByName(source_name);
                block.insert({source_column.column, source_column.type, result_name});
            }

            break;
    }
}


std::string ExpressionAction::toString() const
{
    std::stringstream ss;
    switch (type)
    {
        case ADD_COLUMN:
            ss << "ADD " << result_name << " "
                << (result_type ? result_type->getName() : "(no type)") << " "
                << (added_column ? added_column->getName() : "(no column)");
            break;

        case REMOVE_COLUMN:
            ss << "REMOVE " << source_name;
            break;

        case COPY_COLUMN:
            ss << "COPY " << result_name << " = " << source_name;
            if (can_replace)
                ss << " (can replace)";
            break;

        case APPLY_FUNCTION:
            ss << "FUNCTION " << result_name << " " << (is_function_compiled ? "[compiled] " : "")
                << (result_type ? result_type->getName() : "(no type)") << " = "
                << (function_base ? function_base->getName() : "(no function)") << "(";
            for (size_t i = 0; i < argument_names.size(); ++i)
            {
                if (i)
                    ss << ", ";
                ss << argument_names[i];
            }
            ss << ")";
            break;

        case ARRAY_JOIN:
            ss << "ARRAY JOIN " << source_name << " -> " << result_name;
            break;

        case PROJECT: [[fallthrough]];
        case ADD_ALIASES:
            ss << (type == PROJECT ? "PROJECT " : "ADD_ALIASES ");
            for (size_t i = 0; i < projection.size(); ++i)
            {
                if (i)
                    ss << ", ";
                ss << projection[i].first;
                if (!projection[i].second.empty() && projection[i].second != projection[i].first)
                    ss << " AS " << projection[i].second;
            }
            break;
    }

    return ss.str();
}

ExpressionActions::ExpressionActions(const NamesAndTypesList & input_columns_, const Context & context_)
    : input_columns(input_columns_), settings(context_.getSettingsRef())
{
    for (const auto & input_elem : input_columns)
        sample_block.insert(ColumnWithTypeAndName(nullptr, input_elem.type, input_elem.name));

#if USE_EMBEDDED_COMPILER
compilation_cache = context_.getCompiledExpressionCache();
#endif
}

/// For constant columns the columns themselves can be contained in `input_columns_`.
ExpressionActions::ExpressionActions(const ColumnsWithTypeAndName & input_columns_, const Context & context_)
    : settings(context_.getSettingsRef())
{
    for (const auto & input_elem : input_columns_)
    {
        input_columns.emplace_back(input_elem.name, input_elem.type);
        sample_block.insert(input_elem);
    }
#if USE_EMBEDDED_COMPILER
    compilation_cache = context_.getCompiledExpressionCache();
#endif
}

ExpressionActions::~ExpressionActions() = default;

void ExpressionActions::checkLimits(ExecutionContext & execution_context) const
{
    if (max_temporary_non_const_columns)
    {
        size_t non_const_columns = 0;
        for (const auto & column : execution_context.columns)
            if (column.column && !isColumnConst(*column.column))
                ++non_const_columns;

        if (non_const_columns > settings.max_temporary_non_const_columns)
        {
            std::stringstream list_of_non_const_columns;
            for (const auto & column : execution_context.columns)
                if (column.column && !isColumnConst(*column.column))
                    list_of_non_const_columns << "\n" << column.name;

            throw Exception("Too many temporary non-const columns:" + list_of_non_const_columns.str()
                + ". Maximum: " + settings.max_temporary_non_const_columns.toString(),
                ErrorCodes::TOO_MANY_TEMPORARY_NON_CONST_COLUMNS);
        }
    }
}

void ExpressionActions::prependProjectInput()
{
    actions.insert(actions.begin(), ExpressionAction::project(getRequiredColumns()));
}

void ExpressionActions::execute(Block & block, bool dry_run) const
{
    ExecutionContext execution_context
    {
        .input_columns = block.data,
        .num_rows = block.rows(),
    };

    execution_context.columns.reserve(num_columns);

    ColumnNumbers inputs_to_remove;
    inputs_to_remove.reserve(required_columns.size());
    for (const auto & column : required_columns)
    {
        size_t pos = block.getPositionByName(column.name);
        execution_context.columns.emplace_back(std::move(block.getByPosition(pos)));

        if (!sample_block.has(column.name))
            inputs_to_remove.emplace_back(pos);
    }

    execution_context.columns.resize(num_columns);

    for (const auto & action : actions)
    {
        try
        {
            executeAction(action, execution_context, dry_run);
            checkLimits(execution_context);
        }
        catch (Exception & e)
        {
            e.addMessage(fmt::format("while executing '{}'", action.toString()));
            throw;
        }
    }

    std::sort(inputs_to_remove.rbegin(), inputs_to_remove.rend());
    for (auto input : inputs_to_remove)
        block.erase(input);

    for (const auto & action : actions)
    {
        if (!action.is_used_in_result)
            continue;

        auto & column = execution_context.columns[action.result_position];
        column.name = action.node->result_name;

        if (block.has(action.node->result_name))
            block.getByName(action.node->result_name) = std::move(column);
        else
            block.insert(std::move(column));
    }
}

void ExpressionActions::executeAction(const Action & action, ExecutionContext & execution_context, bool dry_run)
{
    auto & columns = execution_context.columns;
    auto & num_rows = execution_context.num_rows;

    switch (action.node->type)
    {
        case ActionsDAG::Type::FUNCTION:
        {
            auto & res_column = columns[action.result_position];
            if (res_column.type || res_column.column)
                throw Exception("Result column is not empty", ErrorCodes::LOGICAL_ERROR);

            res_column.type = action.node->result_type;
            res_column.name = action.node->result_name;

            ColumnsWithTypeAndName arguments(action.arguments.size());
            for (size_t i = 0; i < arguments.size(); ++i)
                arguments[i] = std::move(columns[action.arguments[i].pos]);

            ProfileEvents::increment(ProfileEvents::FunctionExecute);
            if (action.node->is_function_compiled)
                ProfileEvents::increment(ProfileEvents::CompiledFunctionExecute);

            res_column.column = action.node->function->execute(arguments, res_column.type, num_rows, dry_run);

            for (size_t i = 0; i < arguments.size(); ++i)
                if (!action.arguments[i].remove)
                    arguments[i] = std::move(columns[action.arguments[i].pos]);

            break;
        }

        case ActionsDAG::Type::ARRAY_JOIN:
        {
            size_t array_join_key_pos = action.arguments.front().pos;
            auto array_join_key = columns[array_join_key_pos];

            /// Remove array join argument in advance if it is not needed.
            if (action.arguments.front().remove)
                columns[array_join_key_pos] = {};

            array_join_key.column = array_join_key.column->convertToFullColumnIfConst();

            const ColumnArray * array = typeid_cast<const ColumnArray *>(array_join_key.column.get());
            if (!array)
                throw Exception("ARRAY JOIN of not array: " + action.node->result_name, ErrorCodes::TYPE_MISMATCH);

            for (auto & column : columns)
                if (column.column)
                    column.column = column.column->replicate(array->getOffsets());

            for (auto & column : execution_context.input_columns)
                if (column.column)
                    column.column = column.column->replicate(array->getOffsets());

            auto & res_column = columns[action.result_position];

            res_column.column = array->getDataPtr();
            res_column.type = assert_cast<const DataTypeArray &>(*array_join_key.type).getNestedType();

            num_rows = res_column.column->size();
            break;
        }

        case ActionsDAG::Type::COLUMN:
        {
            auto & res_column = columns[action.result_position];
            res_column.column = action.node->column->cloneResized(num_rows);
            res_column.type = action.node->result_type;
            break;
        }

        case ActionsDAG::Type::ALIAS:
        {
            const auto & arg = action.arguments.front();
            if (action.result_position != arg.pos)
            {
                columns[action.result_position].column = columns[arg.pos].column;
                columns[action.result_position].type = columns[arg.pos].type;
            }

            columns[action.result_position].name = action.node->result_name;

            if (arg.remove)
                columns[arg.pos] = {};

            break;
        }

        case ActionsDAG::Type::INPUT:
        {
            throw Exception("Cannot execute INPUT action", ErrorCodes::LOGICAL_ERROR);
        }
    }
}

Names ExpressionActions::getRequiredColumns() const
{
    Names names;
    for (const auto & input : required_columns)
        names.push_back(input.name);
    return names;
}

bool ExpressionActions::hasArrayJoin() const
{
    for (const auto & action : actions)
        if (action.node->type == ActionsDAG::Type::ARRAY_JOIN)
            return true;

    return false;
}


std::string ExpressionActions::getSmallestColumn(const NamesAndTypesList & columns)
{
    std::optional<size_t> min_size;
    String res;

    for (const auto & column : columns)
    {
        /// @todo resolve evil constant
        size_t size = column.type->haveMaximumSizeOfValue() ? column.type->getMaximumSizeOfValueInMemory() : 100;

        if (!min_size || size < *min_size)
        {
            min_size = size;
            res = column.name;
        }
    }

    if (!min_size)
        throw Exception("No available columns", ErrorCodes::LOGICAL_ERROR);

    return res;
}

void ExpressionActions::finalize(const Names & output_columns)
{
    NameSet final_columns;
    for (const auto & name : output_columns)
    {
        if (!sample_block.has(name))
            throw Exception("Unknown column: " + name + ", there are only columns "
                            + sample_block.dumpNames(), ErrorCodes::UNKNOWN_IDENTIFIER);
        final_columns.insert(name);
    }

#if USE_EMBEDDED_COMPILER
    /// This has to be done before removing redundant actions and inserting REMOVE_COLUMNs
    /// because inlining may change dependency sets.
    if (settings.compile_expressions)
        compileFunctions(actions, output_columns, sample_block, compilation_cache, settings.min_count_to_compile_expression);
#endif

    /// Which columns are needed to perform actions from the current to the last.
    NameSet needed_columns = final_columns;
    /// Which columns nobody will touch from the current action to the last.
    NameSet unmodified_columns;

    {
        NamesAndTypesList sample_columns = sample_block.getNamesAndTypesList();
        for (const auto & sample_column : sample_columns)
            unmodified_columns.insert(sample_column.name);
    }

    /// Let's go from the end and maintain set of required columns at this stage.
    /// We will throw out unnecessary actions, although usually they are absent by construction.
    for (int i = static_cast<int>(actions.size()) - 1; i >= 0; --i)
    {
        ExpressionAction & action = actions[i];
        Names in = action.getNeededColumns();

        if (action.type == ExpressionAction::PROJECT)
        {
            needed_columns = NameSet(in.begin(), in.end());
            unmodified_columns.clear();
        }
        else if (action.type == ExpressionAction::ADD_ALIASES)
        {
            needed_columns.insert(in.begin(), in.end());
            for (auto & name_wit_alias : action.projection)
            {
                auto it = unmodified_columns.find(name_wit_alias.second);
                if (it != unmodified_columns.end())
                    unmodified_columns.erase(it);
            }
        }
        else if (action.type == ExpressionAction::ARRAY_JOIN)
        {
            /// We need source anyway, in order to calculate number of rows correctly.
            needed_columns.insert(action.source_name);
            unmodified_columns.erase(action.result_name);
            needed_columns.erase(action.result_name);

            /// Note: technically, if result of arrayJoin is not needed,
            /// we may remove all the columns and loose the number of rows here.
            /// However, I cannot imagine how it is possible.
            /// For "big" ARRAY JOIN it could have happened in query like
            ///    SELECT count() FROM table ARRAY JOIN x
            /// Now, "big" ARRAY JOIN is moved to separate pipeline step,
            /// and arrayJoin(x) is an expression which result can't be lost.
        }
        else
        {
            std::string out = action.result_name;
            if (!out.empty())
            {
                /// If the result is not used and there are no side effects, throw out the action.
                if (!needed_columns.count(out) &&
                    (action.type == ExpressionAction::APPLY_FUNCTION
                    || action.type == ExpressionAction::ADD_COLUMN
                    || action.type == ExpressionAction::COPY_COLUMN))
                {
                    actions.erase(actions.begin() + i);

                    if (unmodified_columns.count(out))
                    {
                        sample_block.erase(out);
                        unmodified_columns.erase(out);
                    }

                    continue;
                }

                unmodified_columns.erase(out);
                needed_columns.erase(out);

                /** If the function is a constant expression, then replace the action by adding a column-constant - result.
                  * That is, we perform constant folding.
                  */
                if (action.type == ExpressionAction::APPLY_FUNCTION && sample_block.has(out))
                {
                    auto & result = sample_block.getByName(out);
                    if (result.column && names_not_for_constant_folding.count(result.name) == 0)
                    {
                        action.type = ExpressionAction::ADD_COLUMN;
                        action.result_type = result.type;
                        action.added_column = result.column;
                        action.function_builder = nullptr;
                        action.function_base = nullptr;
                        action.function = nullptr;
                        action.argument_names.clear();
                        in.clear();
                    }
                }
            }

            needed_columns.insert(in.begin(), in.end());
        }
    }


    /// 1) Sometimes we don't need any columns to perform actions and sometimes actions doesn't produce any columns as result.
    /// But Block class doesn't store any information about structure itself, it uses information from column.
    /// If we remove all columns from input or output block we will lose information about amount of rows in it.
    /// To avoid this situation we always leaving one of the columns in required columns (input)
    /// and output column. We choose that "redundant" column by size with help of getSmallestColumn.
    ///
    /// 2) Sometimes we have to read data from different Storages to execute query.
    /// For example in 'remote' function which requires to read data from local table (for example MergeTree) and
    /// remote table (doesn't know anything about it).
    ///
    /// If we have combination of two previous cases, our heuristic from (1) can choose absolutely different columns,
    /// so generated streams with these actions will have different headers. To avoid this we additionally rename our "redundant" column
    /// to DUMMY_COLUMN_NAME with help of COPY_COLUMN action and consequent remove of original column.
    /// It doesn't affect any logic, but all streams will have same "redundant" column in header called "_dummy".

    /// Also, it seems like we will always have same type (UInt8) of "redundant" column, but it's not obvious.

    bool dummy_column_copied = false;


    /// We will not throw out all the input columns, so as not to lose the number of rows in the block.
    if (needed_columns.empty() && !input_columns.empty())
    {
        auto colname = getSmallestColumn(input_columns);
        needed_columns.insert(colname);
        actions.insert(actions.begin(), ExpressionAction::copyColumn(colname, DUMMY_COLUMN_NAME, true));
        dummy_column_copied = true;
    }

    /// We will not leave the block empty so as not to lose the number of rows in it.
    if (final_columns.empty() && !input_columns.empty())
    {
        auto colname = getSmallestColumn(input_columns);
        final_columns.insert(DUMMY_COLUMN_NAME);
        if (!dummy_column_copied) /// otherwise we already have this column
            actions.insert(actions.begin(), ExpressionAction::copyColumn(colname, DUMMY_COLUMN_NAME, true));
    }

    for (NamesAndTypesList::iterator it = input_columns.begin(); it != input_columns.end();)
    {
        NamesAndTypesList::iterator it0 = it;
        ++it;
        if (!needed_columns.count(it0->name))
        {
            if (unmodified_columns.count(it0->name))
                sample_block.erase(it0->name);
            input_columns.erase(it0);
        }
    }

/*    std::cerr << "\n";
      for (const auto & action : actions)
          std::cerr << action.toString() << "\n";
      std::cerr << "\n";*/

    /// Deletes unnecessary temporary columns.

    /// If the column after performing the function `refcount = 0`, it can be deleted.
    std::map<String, int> columns_refcount;

    for (const auto & name : final_columns)
        ++columns_refcount[name];

    for (const auto & action : actions)
    {
        if (!action.source_name.empty())
            ++columns_refcount[action.source_name];

        for (const auto & name : action.argument_names)
            ++columns_refcount[name];

        for (const auto & name_alias : action.projection)
            ++columns_refcount[name_alias.first];
    }

    Actions new_actions;
    new_actions.reserve(actions.size());

    for (const auto & action : actions)
    {
        new_actions.push_back(action);

        auto process = [&] (const String & name)
        {
            auto refcount = --columns_refcount[name];
            if (refcount <= 0 && action.type != ExpressionAction::ARRAY_JOIN)
            {
                new_actions.push_back(ExpressionAction::removeColumn(name));
                if (sample_block.has(name))
                    sample_block.erase(name);
            }
        };

        if (!action.source_name.empty())
            process(action.source_name);

        for (const auto & name : action.argument_names)
            process(name);

        /// For `projection`, there is no reduction in `refcount`, because the `project` action replaces the names of the columns, in effect, already deleting them under the old names.
    }

    actions.swap(new_actions);

/*    std::cerr << "\n";
    for (const auto & action : actions)
        std::cerr << action.toString() << "\n";
    std::cerr << "\n";*/

    optimizeArrayJoin();
    checkLimits(sample_block);
}


std::string ExpressionActions::dumpActions() const
{
    std::stringstream ss;

    ss << "input:\n";
    for (const auto & input_column : input_columns)
        ss << input_column.name << " " << input_column.type->getName() << "\n";

    ss << "\nactions:\n";
    for (const auto & action : actions)
        ss << action.toString() << '\n';

    ss << "\noutput:\n";
    NamesAndTypesList output_columns = sample_block.getNamesAndTypesList();
    for (const auto & output_column : output_columns)
        ss << output_column.name << " " << output_column.type->getName() << "\n";

    return ss.str();
}

ExpressionActionsPtr ExpressionActions::splitActionsBeforeArrayJoin(const NameSet & array_joined_columns)
{
    /// Create new actions.
    /// Copy from this because we don't have context.
    /// TODO: remove context from constructor?
    auto split_actions = std::make_shared<ExpressionActions>(*this);
    split_actions->actions.clear();
    split_actions->sample_block.clear();
    split_actions->input_columns.clear();

    /// Expected chain:
    /// Expression (this) -> ArrayJoin (array_joined_columns) -> Expression (split_actions)

    /// We are going to move as many actions as we can from this to split_actions.
    /// We can move all inputs which are not depend on array_joined_columns
    /// (with some exceptions to PROJECT and REMOVE_COLUMN

    /// Use the same inputs for split_actions, except array_joined_columns.
    for (const auto & input_column : input_columns)
    {
        if (array_joined_columns.count(input_column.name) == 0)
        {
            split_actions->input_columns.emplace_back(input_column);
            split_actions->sample_block.insert(ColumnWithTypeAndName(nullptr, input_column.type, input_column.name));
        }
    }

    /// Do not split action if input depends only on array joined columns.
    if (split_actions->input_columns.empty())
        return nullptr;

    /// Actions which depend on ARRAY JOIN result.
    NameSet array_join_dependent_columns = array_joined_columns;
    /// Arguments of actions which depend on ARRAY JOIN result.
    /// This columns can't be deleted in split_actions.
    NameSet array_join_dependent_columns_arguments;

    /// We create new_actions list for `this`. Current actions are moved to new_actions nor added to split_actions.
    Actions new_actions;
    for (const auto & action : actions)
    {
        /// Exception for PROJECT.
        /// It removes columns, so it will remove split_actions output which may be needed for actions from `this`.
        /// So, we replace it ADD_ALIASES.
        /// Usually, PROJECT is added to begin of actions in order to remove unused output of prev actions.
        /// We skip it now, but will prependProjectInput at the end.
        if (action.type == ExpressionAction::PROJECT)
        {
            /// Each alias has separate dependencies, so we split this action into two parts.
            NamesWithAliases split_aliases;
            NamesWithAliases depend_aliases;
            for (const auto & pair : action.projection)
            {
                /// Skip if is not alias.
                if (pair.second.empty())
                    continue;

                if (array_join_dependent_columns.count(pair.first))
                {
                    array_join_dependent_columns.insert(pair.second);
                    depend_aliases.emplace_back(std::move(pair));
                }
                else
                    split_aliases.emplace_back(std::move(pair));
            }

            if (!split_aliases.empty())
                split_actions->add(ExpressionAction::addAliases(split_aliases));

            if (!depend_aliases.empty())
                new_actions.emplace_back(ExpressionAction::addAliases(depend_aliases));

            continue;
        }

        bool depends_on_array_join = false;
        for (auto & column : action.getNeededColumns())
            if (array_join_dependent_columns.count(column) != 0)
                depends_on_array_join = true;

        if (depends_on_array_join)
        {
            /// Add result of this action to array_join_dependent_columns too.
            if (!action.result_name.empty())
                array_join_dependent_columns.insert(action.result_name);

            /// Add arguments of this action to array_join_dependent_columns_arguments.
            auto needed = action.getNeededColumns();
            array_join_dependent_columns_arguments.insert(needed.begin(), needed.end());

            new_actions.emplace_back(action);
        }
        else if (action.type == ExpressionAction::REMOVE_COLUMN)
        {
            /// Exception for REMOVE_COLUMN.
            /// We cannot move it to split_actions if any argument from `this` needed that column.
            if (array_join_dependent_columns_arguments.count(action.source_name))
                new_actions.emplace_back(action);
            else
                split_actions->add(action);
        }
        else
            split_actions->add(action);
    }

    /// Return empty actions if nothing was separated. Keep `this` unchanged.
    if (split_actions->getActions().empty())
        return nullptr;

    std::swap(actions, new_actions);

    /// Collect inputs from ARRAY JOIN.
    NamesAndTypesList inputs_from_array_join;
    for (auto & column : input_columns)
        if (array_joined_columns.count(column.name))
            inputs_from_array_join.emplace_back(std::move(column));

    /// Fix inputs for `this`.
    /// It is output of split_actions + inputs from ARRAY JOIN.
    input_columns = split_actions->getSampleBlock().getNamesAndTypesList();
    input_columns.insert(input_columns.end(), inputs_from_array_join.begin(), inputs_from_array_join.end());

    return split_actions;
}


bool ExpressionActions::checkColumnIsAlwaysFalse(const String & column_name) const
{
    /// Check has column in (empty set).
    String set_to_check;

    for (auto it = actions.rbegin(); it != actions.rend(); ++it)
    {
        const auto & action = *it;
        if (action.node->type == ActionsDAG::Type::FUNCTION && action.node->function_base)
        {
            if (action.node->result_name == column_name && action.node->children.size() > 1)
            {
                auto name = action.node->function_base->getName();
                if ((name == "in" || name == "globalIn"))
                {
                    set_to_check = action.node->children[1]->result_name;
                    break;
                }
            }
        }
    }

    if (!set_to_check.empty())
    {
        for (const auto & action : actions)
        {
            if (action.node->type == ActionsDAG::Type::COLUMN && action.node->result_name == set_to_check)
            {
                // Constant ColumnSet cannot be empty, so we only need to check non-constant ones.
                if (const auto * column_set = checkAndGetColumn<const ColumnSet>(action.node->column.get()))
                {
                    if (column_set->getData()->isCreated() && column_set->getData()->getTotalRowCount() == 0)
                        return true;
                }
            }
        }
    }

    return false;
}


/// It is not important to calculate the hash of individual strings or their concatenation
UInt128 ExpressionAction::ActionHash::operator()(const ExpressionAction & action) const
{
    SipHash hash;
    hash.update(action.type);
    hash.update(action.is_function_compiled);
    switch (action.type)
    {
        case ADD_COLUMN:
            hash.update(action.result_name);
            if (action.result_type)
                hash.update(action.result_type->getName());
            if (action.added_column)
                hash.update(action.added_column->getName());
            break;
        case REMOVE_COLUMN:
            hash.update(action.source_name);
            break;
        case COPY_COLUMN:
            hash.update(action.result_name);
            hash.update(action.source_name);
            break;
        case APPLY_FUNCTION:
            hash.update(action.result_name);
            if (action.result_type)
                hash.update(action.result_type->getName());
            if (action.function_base)
            {
                hash.update(action.function_base->getName());
                for (const auto & arg_type : action.function_base->getArgumentTypes())
                    hash.update(arg_type->getName());
            }
            for (const auto & arg_name : action.argument_names)
                hash.update(arg_name);
            break;
        case ARRAY_JOIN:
            hash.update(action.result_name);
            hash.update(action.source_name);
            break;
        case PROJECT:
            for (const auto & pair_of_strs : action.projection)
            {
                hash.update(pair_of_strs.first);
                hash.update(pair_of_strs.second);
            }
            break;
        case ADD_ALIASES:
            break;
    }
    UInt128 result;
    hash.get128(result.low, result.high);
    return result;
}

bool ExpressionAction::operator==(const ExpressionAction & other) const
{
    if (result_type != other.result_type)
    {
        if (result_type == nullptr || other.result_type == nullptr)
            return false;
        else if (!result_type->equals(*other.result_type))
            return false;
    }

    if (function_base != other.function_base)
    {
        if (function_base == nullptr || other.function_base == nullptr)
            return false;
        else if (function_base->getName() != other.function_base->getName())
            return false;

        const auto & my_arg_types = function_base->getArgumentTypes();
        const auto & other_arg_types = other.function_base->getArgumentTypes();
        if (my_arg_types.size() != other_arg_types.size())
            return false;

        for (size_t i = 0; i < my_arg_types.size(); ++i)
            if (!my_arg_types[i]->equals(*other_arg_types[i]))
                return false;
    }

    if (added_column != other.added_column)
    {
        if (added_column == nullptr || other.added_column == nullptr)
            return false;
        else if (added_column->getName() != other.added_column->getName())
            return false;
    }

    return source_name == other.source_name
        && result_name == other.result_name
        && argument_names == other.argument_names
        && TableJoin::sameJoin(table_join.get(), other.table_join.get())
        && projection == other.projection
        && is_function_compiled == other.is_function_compiled;
}

void ExpressionActionsChain::addStep()
{
    if (steps.empty())
        throw Exception("Cannot add action to empty ExpressionActionsChain", ErrorCodes::LOGICAL_ERROR);

    if (auto * step = typeid_cast<ExpressionActionsStep *>(steps.back().get()))
    {
        if (!step->actions)
            step->actions = step->actions_dag->buildExpressions(context);
    }

    ColumnsWithTypeAndName columns = steps.back()->getResultColumns();
    steps.push_back(std::make_unique<ExpressionActionsStep>(std::make_shared<ActionsDAG>(columns)));
}

void ExpressionActionsChain::finalize()
{
    /// Finalize all steps. Right to left to define unnecessary input columns.
    for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i)
    {
        Names required_output = steps[i]->required_output;
        std::unordered_map<String, size_t> required_output_indexes;
        for (size_t j = 0; j < required_output.size(); ++j)
            required_output_indexes[required_output[j]] = j;
        auto & can_remove_required_output = steps[i]->can_remove_required_output;

        if (i + 1 < static_cast<int>(steps.size()))
        {
            const NameSet & additional_input = steps[i + 1]->additional_input;
            for (const auto & it : steps[i + 1]->getRequiredColumns())
            {
                if (additional_input.count(it.name) == 0)
                {
                    auto iter = required_output_indexes.find(it.name);
                    if (iter == required_output_indexes.end())
                        required_output.push_back(it.name);
                    else if (!can_remove_required_output.empty())
                        can_remove_required_output[iter->second] = false;
                }
            }
        }
        steps[i]->finalize(required_output);
    }

    /// Adding the ejection of unnecessary columns to the beginning of each step.
    for (size_t i = 1; i < steps.size(); ++i)
    {
        size_t columns_from_previous = steps[i - 1]->getResultColumns().size();

        /// If unnecessary columns are formed at the output of the previous step, we'll add them to the beginning of this step.
        /// Except when we drop all the columns and lose the number of rows in the block.
        if (!steps[i]->getResultColumns().empty()
            && columns_from_previous > steps[i]->getRequiredColumns().size())
            steps[i]->prependProjectInput();
    }
}

std::string ExpressionActionsChain::dumpChain() const
{
    std::stringstream ss;

    for (size_t i = 0; i < steps.size(); ++i)
    {
        ss << "step " << i << "\n";
        ss << "required output:\n";
        for (const std::string & name : steps[i]->required_output)
            ss << name << "\n";
        ss << "\n" << steps[i]->dump() << "\n";
    }

    return ss.str();
}

ExpressionActionsChain::ArrayJoinStep::ArrayJoinStep(ArrayJoinActionPtr array_join_, ColumnsWithTypeAndName required_columns_)
    : Step({})
    , array_join(std::move(array_join_))
    , result_columns(std::move(required_columns_))
{
    for (auto & column : result_columns)
    {
        required_columns.emplace_back(NameAndTypePair(column.name, column.type));

        if (array_join->columns.count(column.name) > 0)
        {
            const auto * array = typeid_cast<const DataTypeArray *>(column.type.get());
            column.type = array->getNestedType();
            /// Arrays are materialized
            column.column = nullptr;
        }
    }
}

void ExpressionActionsChain::ArrayJoinStep::finalize(const Names & required_output_)
{
    NamesAndTypesList new_required_columns;
    ColumnsWithTypeAndName new_result_columns;

    NameSet names(required_output_.begin(), required_output_.end());
    for (const auto & column : result_columns)
    {
        if (array_join->columns.count(column.name) != 0 || names.count(column.name) != 0)
            new_result_columns.emplace_back(column);
    }
    for (const auto & column : required_columns)
    {
        if (array_join->columns.count(column.name) != 0 || names.count(column.name) != 0)
            new_required_columns.emplace_back(column);
    }

    std::swap(required_columns, new_required_columns);
    std::swap(result_columns, new_result_columns);
}

ExpressionActionsChain::JoinStep::JoinStep(
    std::shared_ptr<TableJoin> analyzed_join_,
    JoinPtr join_,
    ColumnsWithTypeAndName required_columns_)
    : Step({})
    , analyzed_join(std::move(analyzed_join_))
    , join(std::move(join_))
    , result_columns(std::move(required_columns_))
{
    for (const auto & column : result_columns)
        required_columns.emplace_back(column.name, column.type);

    analyzed_join->addJoinedColumnsAndCorrectNullability(result_columns);
}

void ExpressionActionsChain::JoinStep::finalize(const Names & required_output_)
{
    /// We need to update required and result columns by removing unused ones.
    NamesAndTypesList new_required_columns;
    ColumnsWithTypeAndName new_result_columns;

    /// That's an input columns we need.
    NameSet required_names(required_output_.begin(), required_output_.end());
    for (const auto & name : analyzed_join->keyNamesLeft())
        required_names.emplace(name);

    for (const auto & column : required_columns)
    {
        if (required_names.count(column.name) != 0)
            new_required_columns.emplace_back(column);
    }

    /// Result will also contain joined columns.
    for (const auto & column : analyzed_join->columnsAddedByJoin())
        required_names.emplace(column.name);

    for (const auto & column : result_columns)
    {
        if (required_names.count(column.name) != 0)
            new_result_columns.emplace_back(column);
    }

    std::swap(required_columns, new_required_columns);
    std::swap(result_columns, new_result_columns);
}

ActionsDAGPtr & ExpressionActionsChain::Step::actions()
{
    return typeid_cast<ExpressionActionsStep *>(this)->actions_dag;
}

const ActionsDAGPtr & ExpressionActionsChain::Step::actions() const
{
    return typeid_cast<const ExpressionActionsStep *>(this)->actions_dag;
}

ExpressionActionsPtr ExpressionActionsChain::Step::getExpression() const
{
    return typeid_cast<const ExpressionActionsStep *>(this)->actions;
}

ActionsDAG::ActionsDAG(const NamesAndTypesList & inputs)
{
    for (const auto & input : inputs)
        addInput(input.name, input.type);
}

ActionsDAG::ActionsDAG(const ColumnsWithTypeAndName & inputs)
{
    for (const auto & input : inputs)
        addInput(input);
}

ActionsDAG::Node & ActionsDAG::addNode(Node node, bool can_replace)
{
    auto it = index.find(node.result_name);
    if (it != index.end() && !can_replace)
        throw Exception("Column '" + node.result_name + "' already exists", ErrorCodes::DUPLICATE_COLUMN);

    auto & res = nodes.emplace_back(std::move(node));

    if (it != index.end())
        it->second->renaming_parent = &res;

    index[res.result_name] = &res;
    return res;
}

ActionsDAG::Node & ActionsDAG::getNode(const std::string & name)
{
    auto it = index.find(name);
    if (it == index.end())
        throw Exception("Unknown identifier: '" + name + "'", ErrorCodes::UNKNOWN_IDENTIFIER);

    return *it->second;
}

const ActionsDAG::Node & ActionsDAG::addInput(std::string name, DataTypePtr type)
{
    Node node;
    node.type = Type::INPUT;
    node.result_type = std::move(type);
    node.result_name = std::move(name);

    return addNode(std::move(node));
}

const ActionsDAG::Node & ActionsDAG::addInput(ColumnWithTypeAndName column)
{
    Node node;
    node.type = Type::INPUT;
    node.result_type = std::move(column.type);
    node.result_name = std::move(column.name);
    node.column = std::move(column.column);

    return addNode(std::move(node));
}

const ActionsDAG::Node & ActionsDAG::addColumn(ColumnWithTypeAndName column)
{
    if (!column.column)
        throw Exception("Cannot add column " + column.name + " because it is nullptr", ErrorCodes::LOGICAL_ERROR);

    Node node;
    node.type = Type::COLUMN;
    node.result_type = std::move(column.type);
    node.result_name = std::move(column.name);
    node.column = std::move(column.column);

    return addNode(std::move(node));
}

const ActionsDAG::Node & ActionsDAG::addAlias(const std::string & name, std::string alias, bool can_replace)
{
    auto & child = getNode(name);

    Node node;
    node.type = Type::ALIAS;
    node.result_type = child.result_type;
    node.result_name = std::move(alias);
    node.column = child.column;
    node.allow_constant_folding = child.allow_constant_folding;
    node.children.emplace_back(&child);

    return addNode(std::move(node), can_replace);
}

const ActionsDAG::Node & ActionsDAG::addArrayJoin(
    const std::string & source_name, std::string result_name, std::string unique_column_name)
{
    auto & child = getNode(source_name);

    const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(child.result_type.get());
    if (!array_type)
        throw Exception("ARRAY JOIN requires array argument", ErrorCodes::TYPE_MISMATCH);

    Node node;
    node.type = Type::ARRAY_JOIN;
    node.result_type = array_type->getNestedType();
    node.result_name = std::move(result_name);
    node.unique_column_name_for_array_join = std::move(unique_column_name);
    node.children.emplace_back(&child);

    return addNode(std::move(node));
}

const ActionsDAG::Node & ActionsDAG::addFunction(
    const FunctionOverloadResolverPtr & function,
    const Names & argument_names,
    std::string result_name,
    const Context & context [[maybe_unused]])
{
    const auto & settings = context.getSettingsRef();
    max_temporary_columns = settings.max_temporary_columns;
    max_temporary_non_const_columns = settings.max_temporary_non_const_columns;

    bool do_compile_expressions = false;
#if USE_EMBEDDED_COMPILER
    do_compile_expressions = settings.compile_expressions;

    if (!compilation_cache)
        compilation_cache = context.getCompiledExpressionCache();
#endif

    size_t num_arguments = argument_names.size();

    Node node;
    node.type = Type::FUNCTION;
    node.function_builder = function;
    node.children.reserve(num_arguments);

    bool all_const = true;
    ColumnsWithTypeAndName arguments(num_arguments);

    for (size_t i = 0; i < num_arguments; ++i)
    {
        auto & child = getNode(argument_names[i]);
        node.children.emplace_back(&child);
        node.allow_constant_folding = node.allow_constant_folding && child.allow_constant_folding;

        ColumnWithTypeAndName argument;
        argument.column = child.column;
        argument.type = child.result_type;

        if (!argument.column || !isColumnConst(*argument.column))
            all_const = false;

        arguments[i] = std::move(argument);
    }

    node.function_base = function->build(arguments);
    node.result_type = node.function_base->getResultType();
    node.function = node.function_base->prepare(arguments);

    /// If all arguments are constants, and function is suitable to be executed in 'prepare' stage - execute function.
    /// But if we compile expressions compiled version of this function maybe placed in cache,
    /// so we don't want to unfold non deterministic functions
    if (all_const && node.function_base->isSuitableForConstantFolding() && (!do_compile_expressions || node.function_base->isDeterministic()))
    {
        size_t num_rows = arguments.empty() ? 0 : arguments.front().column->size();
        auto col = node.function->execute(arguments, node.result_type, num_rows, true);

        /// If the result is not a constant, just in case, we will consider the result as unknown.
        if (isColumnConst(*col))
        {
            /// All constant (literal) columns in block are added with size 1.
            /// But if there was no columns in block before executing a function, the result has size 0.
            /// Change the size to 1.

            if (col->empty())
                col = col->cloneResized(1);

            node.column = std::move(col);
        }
    }

    /// Some functions like ignore() or getTypeName() always return constant result even if arguments are not constant.
    /// We can't do constant folding, but can specify in sample block that function result is constant to avoid
    /// unnecessary materialization.
    if (!node.column && node.function_base->isSuitableForConstantFolding())
    {
        if (auto col = node.function_base->getResultIfAlwaysReturnsConstantAndHasArguments(arguments))
        {
            node.column = std::move(col);
            node.allow_constant_folding = false;
        }
    }

    if (result_name.empty())
    {
        result_name = function->getName() + "(";
        for (size_t i = 0; i < argument_names.size(); ++i)
        {
            if (i)
                result_name += ", ";
            result_name += argument_names[i];
        }
        result_name += ")";
    }

    node.result_name = std::move(result_name);

    return addNode(std::move(node));
}

ColumnsWithTypeAndName ActionsDAG::getResultColumns() const
{
    ColumnsWithTypeAndName result;
    result.reserve(index.size());
    for (const auto & node : nodes)
        if (!node.renaming_parent)
            result.emplace_back(node.column, node.result_type, node.result_name);

    return result;
}

NamesAndTypesList ActionsDAG::getNamesAndTypesList() const
{
    NamesAndTypesList result;
    for (const auto & node : nodes)
        if (!node.renaming_parent)
            result.emplace_back(node.result_name, node.result_type);

    return result;
}

Names ActionsDAG::getNames() const
{
    Names names;
    names.reserve(index.size());
    for (const auto & node : nodes)
        if (!node.renaming_parent)
            names.emplace_back(node.result_name);

    return names;
}

std::string ActionsDAG::dumpNames() const
{
    WriteBufferFromOwnString out;
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        if (it != nodes.begin())
            out << ", ";
        out << it->result_name;
    }
    return out.str();
}

void ActionsDAG::removeUnusedActions(const NameSet & required_names)
{
    for (const auto & name : required_names)
    {
        if (index.count(name) == 0)
            throw Exception(ErrorCodes::UNKNOWN_IDENTIFIER,
                            "Unknown column: {}, there are only columns {}", name, dumpNames());
    }

    std::unordered_set<Node *> visited_nodes;
    std::stack<Node *> stack;

    {
        Index new_index;
        for (const auto &[name, node] : index)
        {
            if (required_names.count(std::string(name)))
            {
                new_index[name] = node;
                visited_nodes.insert(node);
                stack.push(node);
            }
        }

        index.swap(new_index);
    }

    while (!stack.empty())
    {
        auto * node = stack.top();
        stack.pop();

        for (auto * child : node->children)
        {
            if (visited_nodes.count(child) == 0)
            {
                stack.push(child);
                visited_nodes.insert(child);
            }
        }
    }

    nodes.remove_if([&](Node * node) { return visited_nodes.count(node) == 0; });

    /// Parent with the same name could be removed. Clear ptr to it if so.
    for (auto & node : nodes)
        if (node.renaming_parent && visited_nodes.count(node.renaming_parent) == 0)
            node.renaming_parent = nullptr;
}

ExpressionActionsPtr ActionsDAG::buildExpressions()
{
    struct Data
    {
        Node * node = nullptr;
        size_t num_created_children = 0;
        size_t num_expected_children = 0;
        std::vector<Node *> parents;
        Node * renamed_child = nullptr;

        ssize_t position = -1;
        size_t num_created_parents = 0;
        bool used_in_result = false;
    };

    std::vector<Data> data(nodes.size());
    std::unordered_map<Node *, size_t> reverse_index;

    for (auto & node : nodes)
    {
        size_t id = reverse_index.size();
        data[id].node = &node;
        reverse_index[&node] = id;
    }

    std::queue<Node *> ready_nodes;
    std::queue<Node *> ready_array_joins;

    for (auto & node : nodes)
    {
        auto & node_data = data[reverse_index[&node]];
        node_data.num_expected_children += node.children.size();
        node_data.used_in_result = node.renaming_parent == nullptr && index.count(node.result_name);

        for (const auto & child : node.children)
            data[reverse_index[child]].parents.emplace_back(&node);

        if (node.renaming_parent)
        {

            auto & cur = data[reverse_index[node.renaming_parent]];
            cur.renamed_child = &node;
            cur.num_expected_children += 1;
        }
    }

    for (auto & node : nodes)
    {
        if (node.children.empty() && data[reverse_index[&node]].renamed_child == nullptr)
            ready_nodes.emplace(&node);
    }

    auto update_parent = [&](Node * parent)
    {
        auto & cur = data[reverse_index[parent]];
        ++cur.num_created_children;

        if (cur.num_created_children == cur.num_expected_children)
        {
            auto & push_stack = parent->type == Type::ARRAY_JOIN ? ready_array_joins : ready_nodes;
            push_stack.push(parent);
        }
    };

    auto expressions = std::make_shared<ExpressionActions>();
    std::stack<size_t> free_positions;

    while (!ready_nodes.empty() || !ready_array_joins.empty())
    {
        auto & stack = ready_nodes.empty() ? ready_array_joins : ready_nodes;
        Node * node = stack.front();
        stack.pop();

        Names argument_names;
        for (const auto & child : node->children)
            argument_names.emplace_back(child->result_name);

        auto & cur = data[reverse_index[node]];

        size_t free_position = expressions->num_columns;
        if (free_positions.empty())
            ++expressions->num_columns;
        else
        {
            free_position = free_positions.top();
            free_positions.pop();
        }

        cur.position = free_position;

        ExpressionActions::Arguments arguments;
        arguments.reserve(cur.node->children.size());
        for (auto * child : cur.node->children)
        {
            auto & arg = data[reverse_index[child]];

            if (arg.position < 0)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Argument was not calculated for {}", child->result_name);

            ++arg.num_created_parents;

            ExpressionActions::Argument argument;
            argument.pos = arg.position;
            argument.remove = !arg.used_in_result && arg.num_created_parents == arg.parents.size();

            arguments.emplace_back(argument);
        }

        if (node->type == Type::INPUT)
            expressions->required_columns.push_back({node->result_name, node->result_type});
        else
            expressions->actions.push_back({node, arguments, free_position, cur.used_in_result});

        if (cur.used_in_result)
            expressions->sample_block.insert({cur.node->column, cur.node->result_type, cur.node->result_name});

        for (const auto & parent : cur.parents)
            update_parent(parent);

        if (node->renaming_parent)
            update_parent(node->renaming_parent);
    }

    expressions->nodes.swap(nodes);
    index.clear();

    if (max_temporary_columns && expressions->num_columns > max_temporary_columns)
        throw Exception(ErrorCodes::TOO_MANY_TEMPORARY_COLUMNS,
                        "Too many temporary columns: {}. Maximum: {}",
                        dumpNames(), std::to_string(max_temporary_columns));

    expressions->max_temporary_non_const_columns = max_temporary_non_const_columns;

    return expressions;
}

}
