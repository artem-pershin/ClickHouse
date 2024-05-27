#include <base/types.h>
#include <boost/algorithm/string/case_conv.hpp>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include "Common/Exception.h"
#include "DataTypes/DataTypeNullable.h"
#include <string_view>

namespace DB
{

namespace ErrorCodes
{
    extern const int UNEXPECTED_DATA_AFTER_PARSED_VALUE;
    extern const int CANNOT_PARSE_INPUT_ASSERTION_FAILED;
}

enum class ErrorHandling : uint8_t
{
    Exception,
    Zero,
    Null
};

template <typename Impl, ErrorHandling error_handling>
class FunctionFromReadable : public IFunction
{
public:
    static constexpr auto name = Impl::name;

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionFromReadable<Impl, error_handling>>(); }

    String getName() const override { return name; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    bool useDefaultImplementationForConstants() const override { return true; }
    size_t getNumberOfArguments() const override { return 1; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        FunctionArgumentDescriptors args
        {
            {"readable_size", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), nullptr, "String"},
        };
        validateFunctionArgumentTypes(*this, arguments, args);
        DataTypePtr return_type = std::make_shared<DataTypeFloat64>();
        if (error_handling == ErrorHandling::Null) {
            return std::make_shared<DataTypeNullable>(return_type);
        } else {
            return return_type;
        }
        
    }
    

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        auto col_res = ColumnFloat64::create();
        auto & res_data = col_res->getData();

        ColumnUInt8::MutablePtr col_null_map;
        if constexpr (error_handling == ErrorHandling::Null)
            col_null_map = ColumnUInt8::create(input_rows_count, 0);

        for (size_t i = 0; i < input_rows_count; ++i)
        {   
            std::string_view str = arguments[0].column->getDataAt(i).toView();
            try
            {
                auto num_bytes = parseReadableFormat(str);
                res_data.emplace_back(num_bytes);
            }
            catch (...)
            {
                if constexpr (error_handling == ErrorHandling::Exception)
                    throw;
                res_data[i] = 0;
                if constexpr (error_handling == ErrorHandling::Null)
                    col_null_map->getData()[i] = 1;
            }
        }
        if constexpr (error_handling == ErrorHandling::Null)
            return ColumnNullable::create(std::move(col_res), std::move(col_null_map));
        else
            return col_res;
    }


private:

    template <typename Arg>
    void throwException(const int code, const String & msg, Arg arg) const
    {
        throw Exception(code, "Invalid expression for function {} - {} (\"{}\")", getName(), msg, arg);
    }

    Float64 parseReadableFormat(const std::string_view & str) const
    {
        ReadBufferFromString buf(str);
        // tryReadFloatText does seem to not raise any error when there is leading whitespace so we check it explicitly
        skipWhitespaceIfAny(buf);
        if (buf.getPosition() > 0)
            throwException(ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED, "Leading whitespace is not allowed", str);

        Float64 base = 0;
        if (!tryReadFloatTextPrecise(base, buf))    // If we use the default (fast) tryReadFloatText this returns True on garbage input
            throwException(ErrorCodes::CANNOT_PARSE_NUMBER, "Unable to parse readable size numeric component", str);
        skipWhitespaceIfAny(buf);

        String unit;
        readStringUntilWhitespace(unit, buf);
        if (!buf.eof())
            throwException(ErrorCodes::UNEXPECTED_DATA_AFTER_PARSED_VALUE, "Found trailing characters after readable size string", str);
        boost::algorithm::to_lower(unit);
        Float64 scale_factor = Impl::getScaleFactorForUnit(unit);
        return base * scale_factor;
    }
};
}
