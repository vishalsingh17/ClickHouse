#include <AggregateFunctions/parseAggregateFunctionParameters.h>

#include <Parsers/ASTLiteral.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Interpreters/evaluateConstantExpression.h>


namespace DB
{
struct Settings;

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int PARAMETERS_TO_AGGREGATE_FUNCTIONS_MUST_BE_LITERALS;
}

Array getAggregateFunctionParametersArray(const ASTPtr & expression_list, const std::string & error_context, ContextPtr context)
{
    const ASTs & parameters = expression_list->children;
    if (parameters.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Parameters list to aggregate functions cannot be empty");

    Array params_row(parameters.size());

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        ASTPtr literal;
        try
        {
            literal = evaluateConstantExpressionAsLiteral(parameters[i], context);
        }
        catch (Exception & e)
        {
            if (e.code() == ErrorCodes::BAD_ARGUMENTS)
                throw Exception(
                    ErrorCodes::PARAMETERS_TO_AGGREGATE_FUNCTIONS_MUST_BE_LITERALS,
                    "Parameters to aggregate functions must be literals. "
                    "Got parameter '{}'{}",
                    parameters[i]->formatForErrorMessage(),
                    (error_context.empty() ? "" : " (in " + error_context +")"));

            throw;
        }

        params_row[i] = literal->as<ASTLiteral>()->value;
    }

    return params_row;
}


void getAggregateFunctionNameAndParametersArray(
    const std::string & aggregate_function_name_with_params,
    std::string & aggregate_function_name,
    Array & aggregate_function_parameters,
    const std::string & error_context,
    ContextPtr context)
{
    if (aggregate_function_name_with_params.back() != ')')
    {
        aggregate_function_name = aggregate_function_name_with_params;
        aggregate_function_parameters = Array();
        return;
    }

    size_t pos = aggregate_function_name_with_params.find('(');
    if (pos == std::string::npos || pos + 2 >= aggregate_function_name_with_params.size())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} doesn't look like aggregate function name in {}.",
                        aggregate_function_name_with_params, error_context);

    aggregate_function_name = aggregate_function_name_with_params.substr(0, pos);
    std::string parameters_str = aggregate_function_name_with_params.substr(pos + 1, aggregate_function_name_with_params.size() - pos - 2);

    if (aggregate_function_name.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "{} doesn't look like aggregate function name in {}.",
                        aggregate_function_name_with_params, error_context);

    ParserExpressionList params_parser(false);
    ASTPtr args_ast = parseQuery(params_parser,
        parameters_str.data(), parameters_str.data() + parameters_str.size(),
        "parameters of aggregate function in " + error_context,
        0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);

    if (args_ast->children.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Incorrect list of parameters to aggregate function {}",
            aggregate_function_name);

    aggregate_function_parameters = getAggregateFunctionParametersArray(args_ast, error_context, context);
}

}
