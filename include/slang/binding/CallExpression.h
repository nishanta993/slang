//------------------------------------------------------------------------------
//! @file CallExpression.h
//! @brief Definitions for call expressions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include "slang/binding/Constraints.h"
#include "slang/binding/Expression.h"

namespace slang {

/// Represents a subroutine call.
class CallExpression : public Expression {
public:
    struct IteratorCallInfo {
        const Expression* iterExpr = nullptr;
        const ValueSymbol* iterVar = nullptr;
    };

    struct RandomizeCallInfo {
        const Constraint* inlineConstraints = nullptr;
        span<const string_view> constraintRestrictions;
    };

    struct SystemCallInfo {
        not_null<const SystemSubroutine*> subroutine;
        not_null<const Scope*> scope;
        std::variant<std::monostate, IteratorCallInfo, RandomizeCallInfo> extraInfo;

        std::pair<const Expression*, const ValueSymbol*> getIteratorInfo() const;
    };

    using Subroutine = std::variant<const SubroutineSymbol*, SystemCallInfo>;
    Subroutine subroutine;

    CallExpression(const Subroutine& subroutine, const Type& returnType,
                   const Expression* thisClass, span<const Expression*> arguments,
                   LookupLocation lookupLocation, SourceRange sourceRange) :
        Expression(ExpressionKind::Call, returnType, sourceRange),
        subroutine(subroutine), thisClass_(thisClass), arguments_(arguments),
        lookupLocation(lookupLocation) {}

    /// If this call is for a class method, returns the expression representing the
    /// class handle on which the method is being invoked. Otherwise returns nullptr.
    const Expression* thisClass() const { return thisClass_; }

    span<const Expression* const> arguments() const { return arguments_; }
    span<const Expression*> arguments() { return arguments_; }

    bool isSystemCall() const { return subroutine.index() == 1; }

    string_view getSubroutineName() const;
    SubroutineKind getSubroutineKind() const;

    ConstantValue evalImpl(EvalContext& context) const;
    bool verifyConstantImpl(EvalContext& context) const;

    void serializeTo(ASTSerializer& serializer) const;

    static Expression& fromSyntax(Compilation& compilation,
                                  const InvocationExpressionSyntax& syntax,
                                  const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                  const BindContext& context);

    static Expression& fromSyntax(Compilation& compilation,
                                  const ArrayOrRandomizeMethodExpressionSyntax& syntax,
                                  const BindContext& context);

    static Expression& fromLookup(Compilation& compilation, const Subroutine& subroutine,
                                  const Expression* thisClass,
                                  const InvocationExpressionSyntax* syntax,
                                  const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                  SourceRange range, const BindContext& context);

    static Expression& fromArgs(Compilation& compilation, const Subroutine& subroutine,
                                const Expression* thisClass, const ArgumentListSyntax* argSyntax,
                                SourceRange range, const BindContext& context);

    static Expression& fromSystemMethod(Compilation& compilation, const Expression& expr,
                                        const LookupResult::MemberSelector& selector,
                                        const InvocationExpressionSyntax* syntax,
                                        const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                        const BindContext& context);

    static Expression* fromBuiltInMethod(Compilation& compilation, SymbolKind rootKind,
                                         const Expression& expr,
                                         const LookupResult::MemberSelector& selector,
                                         const InvocationExpressionSyntax* syntax,
                                         const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                         const BindContext& context);

    static bool isKind(ExpressionKind kind) { return kind == ExpressionKind::Call; }

    template<typename TVisitor>
    void visitExprs(TVisitor&& visitor) const {
        if (thisClass())
            thisClass()->visit(visitor);

        if (isSystemCall()) {
            auto& extra = std::get<1>(subroutine).extraInfo;
            if (extra.index() == 1) {
                if (auto iterExpr = std::get<1>(extra).iterExpr)
                    iterExpr->visit(visitor);
            }
            else if (extra.index() == 2) {
                if (auto constraints = std::get<2>(extra).inlineConstraints)
                    constraints->visit(visitor);
            }
        }

        for (auto arg : arguments())
            arg->visit(visitor);
    }

private:
    static Expression& fromSyntaxImpl(Compilation& compilation, const ExpressionSyntax& left,
                                      const InvocationExpressionSyntax* invocation,
                                      const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                      const BindContext& context);

    static Expression& createSystemCall(Compilation& compilation,
                                        const SystemSubroutine& subroutine,
                                        const Expression* firstArg,
                                        const InvocationExpressionSyntax* syntax,
                                        const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                        SourceRange range, const BindContext& context,
                                        const Scope* randomizeScope = nullptr);

    static bool checkConstant(EvalContext& context, const SubroutineSymbol& subroutine,
                              SourceRange range);

    const Expression* thisClass_;
    span<const Expression*> arguments_;
    LookupLocation lookupLocation;

    mutable bool inRecursion = false;
};

} // namespace slang