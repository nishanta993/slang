//------------------------------------------------------------------------------
// Parser_expressions.cpp
// Expression-related parsing methods
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/diagnostics/ParserDiags.h"
#include "slang/parsing/Lexer.h"
#include "slang/parsing/Parser.h"
#include "slang/parsing/Preprocessor.h"

namespace slang {

ExpressionSyntax& Parser::parseExpression() {
    return parseSubExpression(ExpressionOptions::None, 0);
}

ExpressionSyntax& Parser::parseMinTypMaxExpression() {
    ExpressionSyntax& first = parseExpression();
    if (!peek(TokenKind::Colon))
        return first;

    auto colon1 = consume();
    auto& typ = parseExpression();
    auto colon2 = expect(TokenKind::Colon);
    auto& max = parseExpression();

    return factory.minTypMaxExpression(first, colon1, typ, colon2, max);
}

ExpressionSyntax& Parser::parseExpressionOrDist(bitmask<ExpressionOptions> options) {
    auto& expr = parseSubExpression(options, 0);
    if (!peek(TokenKind::DistKeyword))
        return expr;

    auto& dist = parseDistConstraintList();
    return factory.expressionOrDist(expr, dist);
}

static bool isNewExpr(const ExpressionSyntax* expr) {
    while (true) {
        if (expr->kind == SyntaxKind::ConstructorName)
            return true;

        if (expr->kind != SyntaxKind::ScopedName)
            return false;

        expr = expr->as<ScopedNameSyntax>().right;
    }
}

ExpressionSyntax& Parser::parseSubExpression(bitmask<ExpressionOptions> options, int precedence) {
    auto dg = setDepthGuard();

    auto current = peek();
    if (isPossibleDelayOrEventControl(current.kind)) {
        auto timingControl = parseTimingControl();
        ASSERT(timingControl);

        auto& expr = factory.timingControlExpression(*timingControl, parseExpression());
        return parsePostfixExpression(expr, options);
    }
    else if (current.kind == TokenKind::TaggedKeyword) {
        // TODO: check for trailing expression
        auto tagged = consume();
        auto member = expect(TokenKind::Identifier);
        return factory.taggedUnionExpression(tagged, member, nullptr);
    }

    ExpressionSyntax* leftOperand;
    SyntaxKind opKind = getUnaryPrefixExpression(current.kind);
    if (opKind != SyntaxKind::Unknown) {
        auto opToken = consume();
        auto attributes = parseAttributes();

        auto& operand = parsePrimaryExpression(options);
        auto& postfix = parsePostfixExpression(operand, options);
        leftOperand = &factory.prefixUnaryExpression(opKind, opToken, attributes, postfix);
    }
    else {
        leftOperand = &parsePrimaryExpression(options);

        // If the primary is a new or scoped new operator we should handle
        // that separately (it doesn't participate in postfix expression parsing).
        if (isNewExpr(leftOperand))
            return parseNewExpression(leftOperand->as<NameSyntax>(), options);

        leftOperand = &parsePostfixExpression(*leftOperand, options);
    }

    options &= ~ExpressionOptions::AllowSuperNewCall;
    return parseBinaryExpression(leftOperand, options, precedence);
}

ExpressionSyntax& Parser::parseBinaryExpression(ExpressionSyntax* left,
                                                bitmask<ExpressionOptions> options,
                                                int precedence) {
    Token current;
    while (true) {
        // either a binary operator, or we're done
        current = peek();
        auto opKind = getBinaryExpression(current.kind);
        if (opKind == SyntaxKind::Unknown)
            break;

        // the implication operator in constraint blocks is special, we don't handle it here
        if (opKind == SyntaxKind::LogicalImplicationExpression &&
            options.has(ExpressionOptions::ConstraintContext)) {
            break;
        }

        // we have to special case '<=', which can be less than or nonblocking assignment depending
        // on context
        if (opKind == SyntaxKind::LessThanEqualExpression &&
            options.has(ExpressionOptions::ProceduralAssignmentContext)) {
            opKind = SyntaxKind::NonblockingAssignmentExpression;
        }
        options &= ~ExpressionOptions::ProceduralAssignmentContext;

        // see if we should take this operator or if it's part of our parent due to precedence
        int newPrecedence = getPrecedence(opKind);
        if (newPrecedence < precedence)
            break;

        // if we have a precedence tie, check associativity
        if (newPrecedence == precedence && !isRightAssociative(opKind))
            break;

        // take the operator
        if (opKind == SyntaxKind::InsideExpression)
            left = &parseInsideExpression(*left);
        else {
            auto opToken = consume();
            auto attributes = parseAttributes();
            auto& rightOperand = parseSubExpression(options, newPrecedence);
            left = &factory.binaryExpression(opKind, *left, opToken, attributes, rightOperand);
        }
    }

    // Handle conditional expressions (and their optional pattern matched predicate).
    // Only do this if we're not already within a conditional pattern context, and if
    // we're at the right precedence level (one lower than a logical-or) to take it.
    int logicalOrPrecedence = getPrecedence(SyntaxKind::LogicalOrExpression);
    if (!options.has(ExpressionOptions::PatternContext) && precedence < logicalOrPrecedence) {
        // If this is the start of a pattern predicate, check whether there's actually a
        // question mark coming up. Otherwise we might be a predicate inside a
        // statement which doesn't need the question.
        bool takeConditional = current.kind == TokenKind::Question;
        if (current.kind == TokenKind::MatchesKeyword || current.kind == TokenKind::TripleAnd)
            takeConditional = isConditionalExpression();

        if (takeConditional) {
            Token question;
            auto& predicate = parseConditionalPredicate(*left, TokenKind::Question, question);
            auto attributes = parseAttributes();
            auto& lhs = parseSubExpression(options, logicalOrPrecedence - 1);
            auto colon = expect(TokenKind::Colon);
            auto& rhs = parseSubExpression(options, logicalOrPrecedence - 1);
            left = &factory.conditionalExpression(predicate, question, attributes, lhs, colon, rhs);
        }
    }

    return *left;
}

ExpressionSyntax& Parser::parsePrimaryExpression(bitmask<ExpressionOptions> options) {
    TokenKind kind = peek().kind;
    switch (kind) {
        case TokenKind::StringLiteral:
        case TokenKind::UnbasedUnsizedLiteral:
        case TokenKind::NullKeyword:
        case TokenKind::OneStep:
        case TokenKind::Dollar: {
            auto literal = consume();
            return factory.literalExpression(getLiteralExpression(literal.kind), literal);
        }
        case TokenKind::TimeLiteral:
            return factory.literalExpression(SyntaxKind::TimeLiteralExpression,
                                             numberParser.parseReal(*this));
        case TokenKind::RealLiteral:
            return factory.literalExpression(SyntaxKind::RealLiteralExpression,
                                             numberParser.parseReal(*this));
        case TokenKind::IntegerLiteral:
        case TokenKind::IntegerBase:
            return parseIntegerExpression(options.has(ExpressionOptions::DisallowVectors));
        case TokenKind::OpenParenthesis: {
            auto openParen = consume();
            auto expr = &parseMinTypMaxExpression();

            auto closeParen = expect(TokenKind::CloseParenthesis);
            return factory.parenthesizedExpression(openParen, *expr, closeParen);
        }
        case TokenKind::ApostropheOpenBrace:
            return parseAssignmentPatternExpression(nullptr);
        case TokenKind::OpenBrace: {
            // several different things this could be:
            // 1. empty queue expression { }
            // 2. streaming concatenation {>> {expr}}
            // 3. multiple concatenation {expr {concat}}
            // 4. concatenation {expr, expr}
            auto openBrace = consume();
            switch (peek().kind) {
                case TokenKind::CloseBrace:
                    return factory.emptyQueueExpression(openBrace, consume());
                case TokenKind::LeftShift:
                case TokenKind::RightShift:
                    return parseStreamConcatenation(openBrace);
                default: {
                    auto& first = parseExpression();
                    if (!peek(TokenKind::OpenBrace))
                        return parseConcatenation(openBrace, &first);
                    else {
                        auto openBraceInner = consume();
                        auto& concat = parseConcatenation(openBraceInner, nullptr);
                        auto closeBrace = expect(TokenKind::CloseBrace);
                        return factory.multipleConcatenationExpression(openBrace, first, concat,
                                                                       closeBrace);
                    }
                }
            }
        }
        case TokenKind::SignedKeyword:
        case TokenKind::UnsignedKeyword:
        case TokenKind::ConstKeyword: {
            auto signing = consume();
            auto apostrophe = expect(TokenKind::Apostrophe);
            auto openParen = expect(TokenKind::OpenParenthesis);
            auto& innerExpr = parseExpression();
            auto closeParen = expect(TokenKind::CloseParenthesis);
            auto& parenExpr = factory.parenthesizedExpression(openParen, innerExpr, closeParen);
            return factory.signedCastExpression(signing, apostrophe, parenExpr);
        }
        case TokenKind::SystemIdentifier:
            return factory.systemName(consume());
        default:
            // possibilities here:
            // 1. data type
            // 2. qualified name
            // 3. implicit class handles
            // 4. any of [1-3] with an assignment pattern
            // 5. any of [1-3] with a cast expression
            // 6. error
            if (isPossibleDataType(kind) && kind != TokenKind::Identifier &&
                kind != TokenKind::UnitSystemName) {

                auto& type = parseDataType();
                if (peek(TokenKind::ApostropheOpenBrace))
                    return parseAssignmentPatternExpression(&type);
                else
                    return type;
            }
            else {
                bitmask<NameOptions> nameOptions = NameOptions::ExpectingExpression;
                if (options.has(ExpressionOptions::SequenceExpr))
                    nameOptions |= NameOptions::SequenceExpr;

                // parseName() will insert a missing identifier token for the error case
                auto& name = parseName(nameOptions);
                if (peek(TokenKind::ApostropheOpenBrace))
                    return parseAssignmentPatternExpression(&factory.namedType(name));
                else {
                    // otherwise just a name expression
                    return name;
                }
            }
    }
}

ExpressionSyntax& Parser::parseIntegerExpression(bool disallowVector) {
    auto result =
        disallowVector ? numberParser.parseSimpleInt(*this) : numberParser.parseInteger(*this);

    if (result.isSimple)
        return factory.literalExpression(SyntaxKind::IntegerLiteralExpression, result.value);

    return factory.integerVectorExpression(result.size, result.base, result.value);
}

void Parser::handleExponentSplit(Token token, size_t offset) {
    SmallVectorSized<Token, 4> split;
    Lexer::splitTokens(alloc, getDiagnostics(), getPP().getSourceManager(), token, offset,
                       getPP().getCurrentKeywordVersion(), split);

    pushTokens(split);
}

ExpressionSyntax& Parser::parseInsideExpression(ExpressionSyntax& expr) {
    auto inside = expect(TokenKind::InsideKeyword);
    auto& list = parseOpenRangeList();
    return factory.insideExpression(expr, inside, list);
}

OpenRangeListSyntax& Parser::parseOpenRangeList() {
    Token openBrace;
    Token closeBrace;
    span<TokenOrSyntax> list;

    parseList<isPossibleOpenRangeElement, isEndOfBracedList>(
        TokenKind::OpenBrace, TokenKind::CloseBrace, TokenKind::Comma, openBrace, list, closeBrace,
        RequireItems::True, diag::ExpectedOpenRangeElement,
        [this] { return &parseOpenRangeElement(); });

    return factory.openRangeList(openBrace, list, closeBrace);
}

ExpressionSyntax& Parser::parseOpenRangeElement() {
    if (!peek(TokenKind::OpenBracket))
        return parseExpression();

    auto openBracket = consume();
    auto& left = parseExpression();
    auto colon = expect(TokenKind::Colon);
    auto& right = parseExpression();
    auto closeBracket = expect(TokenKind::CloseBracket);
    return factory.openRangeExpression(openBracket, left, colon, right, closeBracket);
}

ConcatenationExpressionSyntax& Parser::parseConcatenation(Token openBrace,
                                                          ExpressionSyntax* first) {
    SmallVectorSized<TokenOrSyntax, 8> buffer;
    if (first) {
        // it's possible to have just one element in the concatenation list, so check for a close
        // brace
        buffer.append(first);
        if (peek(TokenKind::CloseBrace))
            return factory.concatenationExpression(openBrace, buffer.copy(alloc), consume());

        buffer.append(expect(TokenKind::Comma));
    }

    Token closeBrace;
    parseList<isPossibleExpressionOrComma, isEndOfBracedList>(
        buffer, TokenKind::CloseBrace, TokenKind::Comma, closeBrace, RequireItems::False,
        diag::ExpectedExpression, [this] { return &parseExpression(); });
    return factory.concatenationExpression(openBrace, buffer.copy(alloc), closeBrace);
}

StreamingConcatenationExpressionSyntax& Parser::parseStreamConcatenation(Token openBrace) {
    auto op = consume();
    ExpressionSyntax* sliceSize = nullptr;
    if (!peek(TokenKind::OpenBrace))
        sliceSize = &parseExpression();

    Token openBraceInner;
    Token closeBraceInner;
    span<TokenOrSyntax> list;

    parseList<isPossibleExpressionOrComma, isEndOfBracedList>(
        TokenKind::OpenBrace, TokenKind::CloseBrace, TokenKind::Comma, openBraceInner, list,
        closeBraceInner, RequireItems::True, diag::ExpectedStreamExpression,
        [this] { return &parseStreamExpression(); });

    auto closeBrace = expect(TokenKind::CloseBrace);
    return factory.streamingConcatenationExpression(openBrace, op, sliceSize, openBraceInner, list,
                                                    closeBraceInner, closeBrace);
}

StreamExpressionSyntax& Parser::parseStreamExpression() {
    auto& expr = parseExpression();

    StreamExpressionWithRangeSyntax* withRange = nullptr;
    if (peek(TokenKind::WithKeyword)) {
        auto with = consume();
        withRange = &factory.streamExpressionWithRange(with, parseElementSelect());
    }

    return factory.streamExpression(expr, withRange);
}

AssignmentPatternExpressionSyntax& Parser::parseAssignmentPatternExpression(DataTypeSyntax* type) {
    auto openBrace = expect(TokenKind::ApostropheOpenBrace);

    // we either have an expression here, or the default keyword for a pattern key
    ExpressionSyntax* firstExpr;
    if (peek(TokenKind::DefaultKeyword)) {
        firstExpr = &factory.literalExpression(SyntaxKind::DefaultPatternKeyExpression, consume());
    }
    else if (peek(TokenKind::CloseBrace)) {
        // This is an empty pattern -- we'll just warn and continue on.
        addDiag(diag::EmptyAssignmentPattern, openBrace.location());

        auto pattern =
            &factory.simpleAssignmentPattern(openBrace, span<TokenOrSyntax>{}, consume());
        return factory.assignmentPatternExpression(type, *pattern);
    }
    else {
        firstExpr = &parseExpression();
    }

    Token closeBrace;
    AssignmentPatternSyntax* pattern;
    SmallVectorSized<TokenOrSyntax, 8> buffer;

    switch (peek().kind) {
        case TokenKind::Colon:
            buffer.append(&parseAssignmentPatternItem(firstExpr));
            if (peek(TokenKind::Comma)) {
                buffer.append(consume());

                parseList<isPossibleExpressionOrCommaOrDefault, isEndOfBracedList>(
                    buffer, TokenKind::CloseBrace, TokenKind::Comma, closeBrace,
                    RequireItems::False, diag::ExpectedAssignmentKey,
                    [this] { return &parseAssignmentPatternItem(nullptr); });
            }
            else {
                closeBrace = expect(TokenKind::CloseBrace);
            }

            pattern =
                &factory.structuredAssignmentPattern(openBrace, buffer.copy(alloc), closeBrace);
            break;
        case TokenKind::OpenBrace: {
            auto innerOpenBrace = consume();

            parseList<isPossibleExpressionOrComma, isEndOfBracedList>(
                buffer, TokenKind::CloseBrace, TokenKind::Comma, closeBrace, RequireItems::True,
                diag::ExpectedExpression, [this] { return &parseExpression(); });
            pattern = &factory.replicatedAssignmentPattern(openBrace, *firstExpr, innerOpenBrace,
                                                           buffer.copy(alloc), closeBrace,
                                                           expect(TokenKind::CloseBrace));
            break;
        }
        case TokenKind::Comma:
            buffer.append(firstExpr);
            buffer.append(consume());

            parseList<isPossibleExpressionOrComma, isEndOfBracedList>(
                buffer, TokenKind::CloseBrace, TokenKind::Comma, closeBrace, RequireItems::True,
                diag::ExpectedExpression, [this] { return &parseExpression(); });
            pattern = &factory.simpleAssignmentPattern(openBrace, buffer.copy(alloc), closeBrace);
            break;
        case TokenKind::CloseBrace:
            buffer.append(firstExpr);
            closeBrace = consume();
            pattern = &factory.simpleAssignmentPattern(openBrace, buffer.copy(alloc), closeBrace);
            break;
        default:
            // This is an error case; let the list handling code get us out of it.
            buffer.append(firstExpr);
            buffer.append(expect(TokenKind::Comma));

            parseList<isPossibleExpressionOrComma, isEndOfBracedList>(
                buffer, TokenKind::CloseBrace, TokenKind::Comma, closeBrace, RequireItems::False,
                diag::ExpectedExpression, [this] { return &parseExpression(); });
            pattern = &factory.simpleAssignmentPattern(openBrace, buffer.copy(alloc), closeBrace);
            break;
    }
    ASSERT(pattern);
    return factory.assignmentPatternExpression(type, *pattern);
}

AssignmentPatternItemSyntax& Parser::parseAssignmentPatternItem(ExpressionSyntax* key) {
    if (!key) {
        if (peek(TokenKind::DefaultKeyword))
            key = &factory.literalExpression(SyntaxKind::DefaultPatternKeyExpression, consume());
        else
            key = &parseExpression();
    }

    auto colon = expect(TokenKind::Colon);
    return factory.assignmentPatternItem(*key, colon, parseExpression());
}

ElementSelectSyntax& Parser::parseElementSelect() {
    auto openBracket = expect(TokenKind::OpenBracket);
    auto selector = parseElementSelector();
    auto closeBracket = expect(TokenKind::CloseBracket);
    return factory.elementSelect(openBracket, selector, closeBracket);
}

SelectorSyntax* Parser::parseElementSelector() {
    if (peek().kind == TokenKind::CloseBracket) {
        return nullptr;
    }
    auto& expr = parseExpression();
    switch (peek().kind) {
        case TokenKind::Colon: {
            auto range = consume();
            return &factory.rangeSelect(SyntaxKind::SimpleRangeSelect, expr, range,
                                        parseExpression());
        }
        case TokenKind::PlusColon: {
            auto range = consume();
            return &factory.rangeSelect(SyntaxKind::AscendingRangeSelect, expr, range,
                                        parseExpression());
        }
        case TokenKind::MinusColon: {
            auto range = consume();
            return &factory.rangeSelect(SyntaxKind::DescendingRangeSelect, expr, range,
                                        parseExpression());
        }
        default:
            return &factory.bitSelect(expr);
    }
}

bool Parser::isSequenceRepetition() {
    switch (peek(1).kind) {
        case TokenKind::Star:
        case TokenKind::Equals:
        case TokenKind::MinusArrow:
            return true;
        case TokenKind::Plus:
            return peek(2).kind == TokenKind::CloseBracket;
        default:
            return false;
    }
}

ExpressionSyntax& Parser::parsePostfixExpression(ExpressionSyntax& lhs,
                                                 bitmask<ExpressionOptions> options) {
    ExpressionSyntax* expr = &lhs;
    while (true) {
        switch (peek().kind) {
            case TokenKind::OpenBracket:
                if (options.has(ExpressionOptions::SequenceExpr) && isSequenceRepetition())
                    return *expr;

                expr = &factory.elementSelectExpression(*expr, parseElementSelect());
                break;
            case TokenKind::Dot: {
                auto dot = consume();
                auto name = expect(TokenKind::Identifier);
                expr = &factory.memberAccessExpression(*expr, dot, name);
                break;
            }
            case TokenKind::OpenParenthesis: {
                bool allowClocking = expr->kind == SyntaxKind::SystemName;
                auto& args = parseArgumentList(/* isParamAssignment */ false, allowClocking);
                expr = &factory.invocationExpression(*expr, nullptr, &args);
                break;
            }
            case TokenKind::DoublePlus:
            case TokenKind::DoubleMinus: {
                // can't have any other postfix expressions after inc/dec
                auto op = consume();
                return factory.postfixUnaryExpression(getUnaryPostfixExpression(op.kind), *expr,
                                                      nullptr, op);
            }
            case TokenKind::Apostrophe: {
                auto apostrophe = consume();
                auto openParen = expect(TokenKind::OpenParenthesis);
                auto& innerExpr = parseExpression();
                auto closeParen = expect(TokenKind::CloseParenthesis);
                auto& parenExpr = factory.parenthesizedExpression(openParen, innerExpr, closeParen);
                expr = &factory.castExpression(*expr, apostrophe, parenExpr);
                break;
            }
            case TokenKind::OpenParenthesisStar: {
                auto attributes = parseAttributes();
                switch (peek().kind) {
                    case TokenKind::DoublePlus:
                    case TokenKind::DoubleMinus: {
                        auto op = consume();
                        return factory.postfixUnaryExpression(getUnaryPostfixExpression(op.kind),
                                                              *expr, attributes, op);
                    }
                    case TokenKind::OpenParenthesis:
                        expr = &factory.invocationExpression(
                            *expr, attributes,
                            &parseArgumentList(/* isParamAssignment */ false,
                                               /* allowClocking */ false));
                        break;
                    default:
                        // otherwise, this has to be a function call without any arguments
                        expr = &factory.invocationExpression(*expr, attributes, nullptr);
                        break;
                }
                break;
            }
            case TokenKind::WithKeyword:
                // If we see bracket right after the with keyword, this is actually part of a stream
                // expression -- return and let the call further up the stack handle it.
                if (peek(1).kind == TokenKind::OpenBracket)
                    return *expr;
                expr = &parseArrayOrRandomizeMethod(*expr);
                break;

                // NOTE: If you add a case here, check whether it needs to be added to
                // isBinaryOrPostfixExpression as well.
            default:
                return *expr;
        }
    }
}

NameSyntax& Parser::parseName() {
    return parseName(NameOptions::None);
}

NameSyntax& Parser::parseName(bitmask<NameOptions> options) {
    NameSyntax* name = &parseNamePart(options | NameOptions::IsFirst);
    options &= ~NameOptions::ExpectingExpression;

    bool usedDot = false;
    bool reportedError = false;
    SyntaxKind previousKind = name->kind;

    auto kind = peek().kind;
    while (kind == TokenKind::Dot || kind == TokenKind::DoubleColon) {
        auto separator = consume();
        if (kind == TokenKind::Dot)
            usedDot = true;
        else if (usedDot && !reportedError) {
            reportedError = true;
            addDiag(diag::InvalidAccessDotColon, separator.location()) << "::"sv
                                                                       << "."sv;
        }

        switch (previousKind) {
            case SyntaxKind::UnitScope:
            case SyntaxKind::LocalScope:
                if (kind != TokenKind::DoubleColon) {
                    addDiag(diag::InvalidAccessDotColon, separator.location()) << "."sv
                                                                               << "::"sv;
                }
                break;
            case SyntaxKind::RootScope:
            case SyntaxKind::ThisHandle:
            case SyntaxKind::SuperHandle:
                if (kind != TokenKind::Dot) {
                    addDiag(diag::InvalidAccessDotColon, separator.location()) << "::"sv
                                                                               << "."sv;
                }
                break;
            case SyntaxKind::ConstructorName:
                addDiag(diag::NewKeywordQualified, separator.location());
                break;
            default:
                break;
        }

        bitmask<NameOptions> nextOptions = options;
        if (previousKind == SyntaxKind::ThisHandle)
            nextOptions |= NameOptions::PreviousWasThis;
        else if (previousKind == SyntaxKind::LocalScope)
            nextOptions |= NameOptions::PreviousWasLocal;

        NameSyntax& rhs = parseNamePart(nextOptions);
        previousKind = rhs.kind;

        name = &factory.scopedName(*name, separator, rhs);
        kind = peek().kind;
    }

    // If we saw $unit, $root, super, or local, make sure the correct token follows it.
    TokenKind expectedKind = TokenKind::Unknown;
    switch (name->kind) {
        case SyntaxKind::UnitScope:
        case SyntaxKind::LocalScope:
            expectedKind = TokenKind::DoubleColon;
            break;
        case SyntaxKind::RootScope:
        case SyntaxKind::SuperHandle:
            expectedKind = TokenKind::Dot;
            break;
        default:
            break;
    }

    if (expectedKind != TokenKind::Unknown) {
        auto separator = expect(expectedKind);
        name = &factory.scopedName(*name, separator, parseNamePart(options));
    }

    return *name;
}

NameSyntax& Parser::parseNamePart(bitmask<NameOptions> options) {
    auto kind = getKeywordNameExpression(peek().kind);
    if (kind != SyntaxKind::Unknown) {
        // This is a keyword name such as "super", "xor", or "new".
        bool isFirst = (options & NameOptions::IsFirst) != 0;
        if (isSpecialMethodName(kind)) {
            // The built-in methods ("xor", "unique", etc) and are not allowed
            // to be the first element in the name.
            if (!isFirst)
                return factory.keywordName(kind, consume());
        }
        else if (kind == SyntaxKind::ConstructorName) {
            // "new" names are always allowed.
            return factory.keywordName(kind, consume());
        }
        else {
            // Otherwise this is "$unit", "$root", "local", "this", "super".
            // These are only allowed to be the first element in a path, except
            // for "super" which can follow "this".
            if (isFirst ||
                (kind == SyntaxKind::SuperHandle && options.has(NameOptions::PreviousWasThis)) ||
                ((kind == SyntaxKind::SuperHandle || kind == SyntaxKind::ThisHandle) &&
                 options.has(NameOptions::PreviousWasLocal))) {
                return factory.keywordName(kind, consume());
            }
        }

        // Otherwise fall through to the handling below to get an error emitted.
    }

    TokenKind next = peek().kind;
    Token identifier;
    if (next == TokenKind::Identifier) {
        identifier = consume();
    }
    else if (next != TokenKind::Dot && next != TokenKind::DoubleColon &&
             options.has(NameOptions::ExpectingExpression)) {
        if (!haveDiagAtCurrentLoc())
            addDiag(diag::ExpectedExpression, peek().location());
        identifier = Token::createMissing(alloc, TokenKind::Identifier, peek().location());
    }
    else {
        identifier = expect(TokenKind::Identifier);
    }

    switch (peek().kind) {
        case TokenKind::Hash: {
            auto parameterValues = parseParameterValueAssignment();
            ASSERT(parameterValues);
            return factory.className(identifier, *parameterValues);
        }
        case TokenKind::OpenBracket: {
            if (options.has(NameOptions::SequenceExpr) && isSequenceRepetition())
                return factory.identifierName(identifier);

            uint32_t index = 1;
            scanTypePart<isSemicolon>(index, TokenKind::OpenBracket, TokenKind::CloseBracket);
            if (!options.has(NameOptions::ForeachName) ||
                peek(index).kind != TokenKind::CloseParenthesis) {

                SmallVectorSized<ElementSelectSyntax*, 4> buffer;
                do {
                    buffer.append(&parseElementSelect());
                } while (peek(TokenKind::OpenBracket));

                return factory.identifierSelectName(identifier, buffer.copy(alloc));
            }
            else {
                return factory.identifierName(identifier);
            }
        }
        default:
            return factory.identifierName(identifier);
    }
}

ParameterValueAssignmentSyntax* Parser::parseParameterValueAssignment() {
    if (!peek(TokenKind::Hash))
        return nullptr;

    auto hash = consume();
    auto& args = parseArgumentList(/* isParamAssignment */ true, /* allowClocking */ false);
    return &factory.parameterValueAssignment(hash, args);
}

ArgumentListSyntax& Parser::parseArgumentList(bool isParamAssignment, bool allowClocking) {
    Token openParen;
    Token closeParen;
    span<TokenOrSyntax> list;

    auto allowEmpty = isParamAssignment ? AllowEmpty::False : AllowEmpty::True;

    parseList<isPossibleArgument, isEndOfParenList>(
        TokenKind::OpenParenthesis, TokenKind::CloseParenthesis, TokenKind::Comma, openParen, list,
        closeParen, RequireItems::False, diag::ExpectedArgument,
        [this, isParamAssignment, allowClocking] {
            return &parseArgument(isParamAssignment, allowClocking);
        },
        allowEmpty);

    return factory.argumentList(openParen, list, closeParen);
}

ArgumentSyntax& Parser::parseArgument(bool isParamAssignment, bool allowClocking) {
    // check for empty arguments
    if (!isParamAssignment && (peek(TokenKind::Comma) || peek(TokenKind::CloseParenthesis)))
        return factory.emptyArgument(placeholderToken());

    // check for named arguments
    if (peek(TokenKind::Dot)) {
        auto dot = consume();
        auto name = expect(TokenKind::Identifier);

        auto [innerOpenParen, innerCloseParen, expr] = parseGroupOrSkip(
            TokenKind::OpenParenthesis, TokenKind::CloseParenthesis, [this, isParamAssignment]() {
                return isParamAssignment ? &parseMinTypMaxExpression() : &parseExpression();
            });

        return factory.namedArgument(dot, name, innerOpenParen, expr, innerCloseParen);
    }

    if (allowClocking && peek(TokenKind::At)) {
        auto timing = parseTimingControl();
        ASSERT(timing);
        return factory.clockingEventArgument(*timing);
    }

    return factory.orderedArgument(isParamAssignment ? parseMinTypMaxExpression()
                                                     : parseExpression());
}

PatternSyntax& Parser::parsePattern() {
    switch (peek().kind) {
        case TokenKind::DotStar:
            return factory.wildcardPattern(consume());
        case TokenKind::Dot: {
            auto dot = consume();
            return factory.variablePattern(dot, expect(TokenKind::Identifier));
        }
        case TokenKind::TaggedKeyword: {
            auto tagged = consume();
            auto name = expect(TokenKind::Identifier);
            // TODO: optional trailing pattern
            return factory.taggedPattern(tagged, name, nullptr);
        }
        case TokenKind::ApostropheOpenBrace:
            // TODO: assignment pattern
            break;
        default:
            break;
    }

    // otherwise, it's either an expression or an error (parseExpression will handle that for us)
    return factory.expressionPattern(parseSubExpression(ExpressionOptions::PatternContext, 0));
}

ConditionalPredicateSyntax& Parser::parseConditionalPredicate(ExpressionSyntax& first,
                                                              TokenKind endKind, Token& end) {
    SmallVectorSized<TokenOrSyntax, 4> buffer;

    MatchesClauseSyntax* matchesClause = nullptr;
    if (peek(TokenKind::MatchesKeyword)) {
        auto matches = consume();
        matchesClause = &factory.matchesClause(matches, parsePattern());
    }

    buffer.append(&factory.conditionalPattern(first, matchesClause));

    if (peek(TokenKind::TripleAnd)) {
        buffer.append(consume());
        parseList<isPossibleExpressionOrTripleAnd, isEndOfConditionalPredicate>(
            buffer, endKind, TokenKind::TripleAnd, end, RequireItems::True,
            diag::ExpectedConditionalPattern, [this] { return &parseConditionalPattern(); });
    }
    else {
        end = expect(endKind);
    }

    return factory.conditionalPredicate(buffer.copy(alloc));
}

ConditionalPatternSyntax& Parser::parseConditionalPattern() {
    auto& expr = parseSubExpression(ExpressionOptions::PatternContext, 0);

    MatchesClauseSyntax* matchesClause = nullptr;
    if (peek(TokenKind::MatchesKeyword)) {
        auto matches = consume();
        matchesClause = &factory.matchesClause(matches, parsePattern());
    }

    return factory.conditionalPattern(expr, matchesClause);
}

EventExpressionSyntax& Parser::parseEventExpression() {
    EventExpressionSyntax* left;
    auto kind = peek().kind;
    if (kind == TokenKind::OpenParenthesis) {
        auto openParen = consume();
        auto& expr = parseEventExpression();
        auto closeParen = expect(TokenKind::CloseParenthesis);
        left = &factory.parenthesizedEventExpression(openParen, expr, closeParen);
    }
    else {
        Token edge = parseEdgeKeyword();
        auto& expr = parseExpression();

        IffEventClauseSyntax* iffClause = nullptr;
        if (peek(TokenKind::IffKeyword)) {
            auto iff = consume();
            auto& iffExpr = parseExpression();
            iffClause = &factory.iffEventClause(iff, iffExpr);
        }

        left = &factory.signalEventExpression(edge, expr, iffClause);
    }

    kind = peek().kind;
    if (kind == TokenKind::Comma || kind == TokenKind::OrKeyword) {
        auto op = consume();
        left = &factory.binaryEventExpression(*left, op, parseEventExpression());
    }
    return *left;
}

ExpressionSyntax& Parser::parseNewExpression(NameSyntax& newKeyword,
                                             bitmask<ExpressionOptions> options) {
    // If we see an open bracket, this is a dynamic array new expression.
    auto kind = peek().kind;
    if (kind == TokenKind::OpenBracket) {
        auto openBracket = consume();
        auto& sizeExpr = parseExpression();
        auto closeBracket = expect(TokenKind::CloseBracket);

        ParenthesizedExpressionSyntax* initializer = nullptr;
        if (peek(TokenKind::OpenParenthesis)) {
            auto openParen = consume();
            auto& initializerExpr = parseExpression();
            initializer = &factory.parenthesizedExpression(openParen, initializerExpr,
                                                           expect(TokenKind::CloseParenthesis));
        }
        return factory.newArrayExpression(newKeyword, openBracket, sizeExpr, closeBracket,
                                          initializer);
    }

    // Enforce rules for super.new placement.
    if (newKeyword.kind == SyntaxKind::ScopedName) {
        auto& scoped = newKeyword.as<ScopedNameSyntax>();
        if (scoped.right->kind == SyntaxKind::ConstructorName &&
            scoped.left->getLastToken().kind == TokenKind::SuperKeyword) {
            if ((options & ExpressionOptions::AllowSuperNewCall) == 0) {
                addDiag(diag::InvalidSuperNew, scoped.right->getFirstToken().location())
                    << newKeyword.sourceRange();
            }
        }
    }

    // Otherwise this is a new-class or copy-class expression.
    // new-class has an optional argument list, copy-class has a required expression.
    // An open paren here would be ambiguous between an arg list and a parenthesized
    // expression -- we resolve by always taking the arg list.
    if (kind == TokenKind::OpenParenthesis) {
        return factory.newClassExpression(
            newKeyword,
            &parseArgumentList(/* isParamAssignment */ false, /* allowClocking */ false));
    }

    if (isPossibleExpression(kind)) {
        if (newKeyword.kind != SyntaxKind::ConstructorName)
            addDiag(diag::ScopedClassCopy, peek().location()) << newKeyword.sourceRange();
        return factory.copyClassExpression(newKeyword, parseExpression());
    }

    return factory.newClassExpression(newKeyword, nullptr);
}

TimingControlSyntax* Parser::parseTimingControl() {
    switch (peek().kind) {
        case TokenKind::Hash:
        case TokenKind::DoubleHash: {
            auto hash = consume();
            auto& delay = parsePrimaryExpression(ExpressionOptions::DisallowVectors);
            auto kind =
                hash.kind == TokenKind::Hash ? SyntaxKind::DelayControl : SyntaxKind::CycleDelay;

            return &factory.delay(kind, hash, delay);
        }
        case TokenKind::At: {
            auto at = consume();
            switch (peek().kind) {
                case TokenKind::OpenParenthesis: {
                    auto openParen = consume();
                    if (peek(TokenKind::Star)) {
                        auto star = consume();
                        return &factory.implicitEventControl(at, openParen, star,
                                                             expect(TokenKind::CloseParenthesis));
                    }

                    auto& eventExpr = parseEventExpression();
                    auto closeParen = expect(TokenKind::CloseParenthesis);
                    return &factory.eventControlWithExpression(
                        at, factory.parenthesizedEventExpression(openParen, eventExpr, closeParen));
                }
                case TokenKind::OpenParenthesisStar: {
                    // Special case since @(*) will be lexed as '@' '(*' ')'
                    auto openParen = consume();
                    return &factory.implicitEventControl(at, openParen, Token(),
                                                         expect(TokenKind::CloseParenthesis));
                }
                case TokenKind::Star:
                    return &factory.implicitEventControl(at, Token(), consume(), Token());
                default:
                    return &factory.eventControl(at, parseName());
            }
        }
        case TokenKind::RepeatKeyword: {
            auto repeat = consume();
            auto openParen = expect(TokenKind::OpenParenthesis);
            auto& expr = parseExpression();
            auto closeParen = expect(TokenKind::CloseParenthesis);
            return &factory.repeatedEventControl(repeat, openParen, expr, closeParen,
                                                 parseTimingControl());
        }
        default:
            return nullptr;
    }
}

ExpressionSyntax& Parser::parseArrayOrRandomizeMethod(ExpressionSyntax& expr) {
    auto with = consume();

    ParenExpressionListSyntax* args = nullptr;
    if (peek(TokenKind::OpenParenthesis)) {
        Token openParen, closeParen;
        span<TokenOrSyntax> items;
        parseList<isPossibleExpressionOrComma, isEndOfParenList>(
            TokenKind::OpenParenthesis, TokenKind::CloseParenthesis, TokenKind::Comma, openParen,
            items, closeParen, RequireItems::False, diag::ExpectedExpression,
            [this] { return &parseExpression(); });

        args = &factory.parenExpressionList(openParen, items, closeParen);
    }

    ConstraintBlockSyntax* constraints = nullptr;
    if (peek(TokenKind::OpenBrace))
        constraints = &parseConstraintBlock(/* isTopLevel */ true);

    return factory.arrayOrRandomizeMethodExpression(expr, with, args, constraints);
}

bool Parser::isConditionalExpression() {
    uint32_t index = 1;
    while (true) {
        TokenKind kind = peek(index++).kind;
        switch (kind) {
            case TokenKind::Question:
                return true;
            case TokenKind::CloseParenthesis:
                return false;
            case TokenKind::OpenParenthesis:
                if (!scanTypePart<isNotInType>(index, TokenKind::OpenParenthesis,
                                               TokenKind::CloseParenthesis)) {
                    return false;
                }
                break;
            case TokenKind::OpenBrace:
                if (!scanTypePart<isNotInType>(index, TokenKind::OpenBrace,
                                               TokenKind::CloseBrace)) {
                    return false;
                }
                break;
            case TokenKind::OpenBracket:
                if (!scanTypePart<isNotInType>(index, TokenKind::OpenBracket,
                                               TokenKind::CloseBracket)) {
                    return false;
                }
                break;
            default:
                if (isNotInType(kind))
                    return false;
                break;
        }
    }
}

SequenceExprSyntax& Parser::parseDelayedSequenceExpr(SequenceExprSyntax* first) {
    SmallVectorSized<DelayedSequenceElementSyntax*, 4> elements;
    do {
        Token op, openBracket, closeBracket;
        SelectorSyntax* selector = nullptr;
        ExpressionSyntax* delayVal = nullptr;

        auto hash = expect(TokenKind::DoubleHash);

        if (peek(TokenKind::OpenBracket)) {
            openBracket = consume();
            if ((peek(TokenKind::Star) || peek(TokenKind::Plus)) &&
                peek(1).kind == TokenKind::CloseBracket) {
                op = consume();
            }
            else {
                selector = parseElementSelector();
            }
            closeBracket = expect(TokenKind::CloseBracket);
        }
        else {
            delayVal = &parsePrimaryExpression(ExpressionOptions::None);
        }

        auto& expr = parseSequencePrimary();
        elements.append(&factory.delayedSequenceElement(hash, delayVal, openBracket, op, selector,
                                                        closeBracket, expr));

    } while (peek(TokenKind::DoubleHash));

    return factory.delayedSequenceExpr(first, elements.copy(alloc));
}

static bool isBinaryOrPostfixExpression(TokenKind kind) {
    // NOTE: This deliberately does not include the open bracket because
    // this function is only called on tokens that occur right after a
    // parenthesized expression ends, in a sequence or property context.
    // In those places, an open bracket means something else.
    switch (kind) {
        case TokenKind::Dot:
        case TokenKind::OpenParenthesis:
        case TokenKind::OpenParenthesisStar:
        case TokenKind::Apostrophe:
        case TokenKind::DistKeyword:
        case TokenKind::Question:
            return true;
        default:
            return SyntaxFacts::getBinaryExpression(kind) != SyntaxKind::Unknown;
    }
}

ExpressionSyntax& Parser::fixParenthesizedExpression(const SimpleSequenceExprSyntax& source,
                                                     Token openParen) {
    ExpressionSyntax* result = source.expr;
    result =
        &factory.parenthesizedExpression(openParen, *result, expect(TokenKind::CloseParenthesis));
    result = &parsePostfixExpression(*result, ExpressionOptions::SequenceExpr);
    result = &parseBinaryExpression(result, ExpressionOptions::SequenceExpr, 0);

    if (!peek(TokenKind::DistKeyword))
        return *result;

    auto& dist = parseDistConstraintList();
    return factory.expressionOrDist(*result, dist);
}

SequenceMatchListSyntax* Parser::parseSequenceMatchList(Token& closeParen) {
    if (!peek(TokenKind::Comma)) {
        closeParen = expect(TokenKind::CloseParenthesis);
        return nullptr;
    }

    Token comma;
    span<TokenOrSyntax> list;
    parseList<isPossibleExpressionOrComma, isEndOfParenList>(
        TokenKind::Comma, TokenKind::CloseParenthesis, TokenKind::Comma, comma, list, closeParen,
        RequireItems::True, diag::ExpectedExpression, [this] { return &parseExpression(); });

    return &factory.sequenceMatchList(comma, list);
}

SequenceRepetitionSyntax* Parser::parseSequenceRepetition() {
    if (!peek(TokenKind::OpenBracket))
        return nullptr;

    auto openBracket = consume();

    Token op;
    switch (peek().kind) {
        case TokenKind::Plus:
        case TokenKind::Equals:
        case TokenKind::MinusArrow:
            op = consume();
            break;
        default:
            op = expect(TokenKind::Star);
            break;
    }

    auto selector = parseElementSelector();
    auto closeBracket = expect(TokenKind::CloseBracket);
    return &factory.sequenceRepetition(openBracket, op, selector, closeBracket);
}

SequenceExprSyntax& Parser::parseSequencePrimary() {
    auto current = peek();
    switch (current.kind) {
        case TokenKind::DoubleHash:
            return parseDelayedSequenceExpr(nullptr);
        case TokenKind::At: {
            auto event = parseTimingControl();
            ASSERT(event);
            return factory.clockingSequenceExpr(*event,
                                                parseSequenceExpr(0, /* isInProperty */ false));
        }
        case TokenKind::FirstMatchKeyword: {
            auto keyword = consume();
            auto openParen = consume();
            auto& expr = parseSequenceExpr(0, /* isInProperty */ false);

            Token closeParen;
            auto matchList = parseSequenceMatchList(closeParen);
            return factory.firstMatchSequenceExpr(keyword, openParen, expr, matchList, closeParen);
        }
        case TokenKind::OpenParenthesis: {
            auto openParen = consume();
            auto& expr = parseSequenceExpr(0, /* isInProperty */ false);

            // There is ambiguity between parenthesized sequence expressions and normal
            // expressions. To resolve, we need to see if we are at the end of the
            // parenthesis and what comes after can only be another piece of the expression.
            if (expr.kind == SyntaxKind::SimpleSequenceExpr && peek(TokenKind::CloseParenthesis) &&
                isBinaryOrPostfixExpression(peek(1).kind)) {
                auto& fixed =
                    fixParenthesizedExpression(expr.as<SimpleSequenceExprSyntax>(), openParen);

                auto repetition = parseSequenceRepetition();
                return factory.simpleSequenceExpr(fixed, repetition);
            }

            Token closeParen;
            auto matchList = parseSequenceMatchList(closeParen);

            auto repetition = parseSequenceRepetition();
            return factory.parenthesizedSequenceExpr(openParen, expr, matchList, closeParen,
                                                     repetition);
        }
        default: {
            auto& expr = parseExpressionOrDist(ExpressionOptions::SequenceExpr);
            auto repetition = parseSequenceRepetition();
            return factory.simpleSequenceExpr(expr, repetition);
        }
    }
}

SequenceExprSyntax& Parser::parseSequenceExpr(int precedence, bool isInProperty) {
    auto dg = setDepthGuard();

    auto left = &parseSequencePrimary();
    if (peek(TokenKind::DoubleHash))
        left = &parseDelayedSequenceExpr(left);

    while (true) {
        // either a binary operator, or we're done
        auto opKind = getBinarySequenceExpr(peek().kind);
        if (opKind == SyntaxKind::Unknown)
            break;

        // Inside a property, we don't consume an "and" or "or" expression because
        // we want the parent property parser to get a chance at it.
        if (isInProperty &&
            (opKind == SyntaxKind::AndSequenceExpr || opKind == SyntaxKind::OrSequenceExpr)) {
            break;
        }

        // see if we should take this operator or if it's part of our parent due to precedence
        int newPrecedence = getPrecedence(opKind);
        if (newPrecedence < precedence)
            break;

        // if we have a precedence tie, check associativity
        if (newPrecedence == precedence && !isRightAssociative(opKind))
            break;

        // take the operator
        auto opToken = consume();
        auto& right = parseSequenceExpr(newPrecedence, isInProperty);
        left = &factory.binarySequenceExpr(opKind, *left, opToken, right);
    }

    return *left;
}

PropertyExprSyntax& Parser::parseCasePropertyExpr() {
    auto keyword = consume();
    auto openParen = expect(TokenKind::OpenParenthesis);
    auto& condition = parseExpressionOrDist();
    auto closeParen = expect(TokenKind::CloseParenthesis);

    SmallVectorSized<PropertyCaseItemSyntax*, 8> itemBuffer;
    SourceLocation lastDefault;
    bool errored = false;

    while (true) {
        auto kind = peek().kind;
        if (kind == TokenKind::DefaultKeyword) {
            if (lastDefault && !errored) {
                auto& diag = addDiag(diag::MultipleDefaultCases, peek().location()) << "case"sv;
                diag.addNote(diag::NotePreviousDefinition, lastDefault);
                errored = true;
            }

            lastDefault = peek().location();

            auto def = consume();
            auto colon = consumeIf(TokenKind::Colon);
            auto& expr = parsePropertyExpr(0);
            auto semi = expect(TokenKind::Semicolon);
            itemBuffer.append(&factory.defaultPropertyCaseItem(def, colon, expr, semi));
        }
        else if (isPossibleExpression(kind)) {
            Token colon;
            SmallVectorSized<TokenOrSyntax, 8> buffer;
            parseList<isPossibleExpressionOrComma, isEndOfCaseItem>(
                buffer, TokenKind::Colon, TokenKind::Comma, colon, RequireItems::True,
                diag::ExpectedExpression, [this] { return &parseExpressionOrDist(); });

            auto& expr = parsePropertyExpr(0);
            auto semi = expect(TokenKind::Semicolon);
            itemBuffer.append(
                &factory.standardPropertyCaseItem(buffer.copy(alloc), colon, expr, semi));
        }
        else {
            break;
        }
    }

    if (itemBuffer.empty())
        addDiag(diag::CaseStatementEmpty, keyword.location()) << "case"sv;

    auto endcase = expect(TokenKind::EndCaseKeyword);
    return factory.casePropertyExpr(keyword, openParen, condition, closeParen,
                                    itemBuffer.copy(alloc), endcase);
}

PropertyExprSyntax& Parser::parsePropertyPrimary() {
    auto current = peek();
    switch (current.kind) {
        case TokenKind::At: {
            auto event = parseTimingControl();
            ASSERT(event);
            return factory.clockingPropertyExpr(*event, parsePropertyExpr(0));
        }
        case TokenKind::OpenParenthesis: {
            auto openParen = consume();
            auto& expr = parsePropertyExpr(0);

            // There is ambiguity between parenthesized property expressions and normal
            // expressions. To resolve, we need to see if we are at the end of the
            // parenthesis and what comes after can only be another piece of the expression.
            if (expr.kind == SyntaxKind::SimplePropertyExpr && peek(TokenKind::CloseParenthesis) &&
                isBinaryOrPostfixExpression(peek(1).kind)) {
                auto& simpProp = expr.as<SimplePropertyExprSyntax>();
                if (simpProp.expr->kind == SyntaxKind::SimpleSequenceExpr) {
                    auto& fixed = fixParenthesizedExpression(
                        simpProp.expr->as<SimpleSequenceExprSyntax>(), openParen);

                    auto& simpSeq = factory.simpleSequenceExpr(fixed, nullptr);
                    return factory.simplePropertyExpr(simpSeq);
                }
            }

            // Similarly, this could have been a parenthesized sequence expression
            // instead, in which case we would fail if there is sequence-specific
            // tokens up next instead of a closing parenthesis.
            if (expr.kind == SyntaxKind::SimplePropertyExpr &&
                (peek(TokenKind::Comma) ||
                 (peek(TokenKind::CloseParenthesis) && peek(1).kind == TokenKind::OpenBracket))) {
                auto& seqExpr = *expr.as<SimplePropertyExprSyntax>().expr;

                Token closeParen;
                auto matchList = parseSequenceMatchList(closeParen);
                auto repetition = parseSequenceRepetition();
                auto& parenSeqExpr = factory.parenthesizedSequenceExpr(
                    openParen, seqExpr, matchList, closeParen, repetition);

                return factory.simplePropertyExpr(parenSeqExpr);
            }

            auto closeParen = expect(TokenKind::CloseParenthesis);
            return factory.parenthesizedPropertyExpr(openParen, expr, closeParen);
        }
        case TokenKind::StrongKeyword:
        case TokenKind::WeakKeyword: {
            auto keyword = consume();
            auto openParen = consume();
            auto& expr = parseSequenceExpr(0, /* isInProperty */ false);
            auto closeParen = expect(TokenKind::CloseParenthesis);
            return factory.strongWeakPropertyExpr(keyword, openParen, expr, closeParen);
        }
        case TokenKind::NotKeyword: {
            auto op = consume();
            auto& expr = parsePropertyPrimary();
            return factory.unaryPropertyExpr(op, expr);
        }
        case TokenKind::NextTimeKeyword:
        case TokenKind::SNextTimeKeyword: {
            auto op = consume();
            if (peek(TokenKind::OpenBracket)) {
                auto openBracket = consume();
                auto selector = parseElementSelector();
                auto closeBracket = expect(TokenKind::CloseBracket);
                auto& expr = parsePropertyPrimary();
                return factory.unarySelectPropertyExpr(op, openBracket, selector, closeBracket,
                                                       expr);
            }

            auto& expr = parsePropertyPrimary();
            return factory.unaryPropertyExpr(op, expr);
        }
        case TokenKind::AlwaysKeyword:
        case TokenKind::SAlwaysKeyword:
        case TokenKind::EventuallyKeyword:
        case TokenKind::SEventuallyKeyword: {
            auto op = consume();
            if (peek(TokenKind::OpenBracket)) {
                auto openBracket = consume();
                auto selector = parseElementSelector();
                auto closeBracket = expect(TokenKind::CloseBracket);
                auto& expr = parsePropertyExpr(0);
                return factory.unarySelectPropertyExpr(op, openBracket, selector, closeBracket,
                                                       expr);
            }

            auto& expr = parsePropertyExpr(0);
            return factory.unaryPropertyExpr(op, expr);
        }
        case TokenKind::AcceptOnKeyword:
        case TokenKind::RejectOnKeyword:
        case TokenKind::SyncAcceptOnKeyword:
        case TokenKind::SyncRejectOnKeyword: {
            auto keyword = consume();
            auto openParen = consume();
            auto& condition = parseExpressionOrDist();
            auto closeParen = expect(TokenKind::CloseParenthesis);
            auto& expr = parsePropertyExpr(0);
            return factory.acceptOnPropertyExpr(keyword, openParen, condition, closeParen, expr);
        }
        case TokenKind::IfKeyword: {
            auto keyword = consume();
            auto openParen = consume();
            auto& condition = parseExpressionOrDist();
            auto closeParen = expect(TokenKind::CloseParenthesis);
            auto& expr = parsePropertyExpr(0);

            ElsePropertyClauseSyntax* elseClause = nullptr;
            if (peek(TokenKind::ElseKeyword)) {
                auto elseKeyword = consume();
                auto& elseExpr = parsePropertyExpr(0);
                elseClause = &factory.elsePropertyClause(elseKeyword, elseExpr);
            }

            return factory.conditionalPropertyExpr(keyword, openParen, condition, closeParen, expr,
                                                   elseClause);
        }
        case TokenKind::CaseKeyword:
            return parseCasePropertyExpr();
        default: {
            auto& expr = parseSequenceExpr(0, /* isInProperty */ true);
            return factory.simplePropertyExpr(expr);
        }
    }
}

PropertyExprSyntax& Parser::parsePropertyExpr(int precedence) {
    auto dg = setDepthGuard();

    auto left = &parsePropertyPrimary();
    while (true) {
        // either a binary operator, or we're done
        auto opKind = getBinaryPropertyExpr(peek().kind);
        if (opKind == SyntaxKind::Unknown)
            break;

        // see if we should take this operator or if it's part of our parent due to precedence
        int newPrecedence = getPrecedence(opKind);
        if (newPrecedence < precedence)
            break;

        // if we have a precedence tie, check associativity
        if (newPrecedence == precedence && !isRightAssociative(opKind))
            break;

        // take the operator
        auto opToken = consume();
        auto& right = parsePropertyExpr(newPrecedence);
        left = &factory.binaryPropertyExpr(opKind, *left, opToken, right);
    }

    return *left;
}

} // namespace slang
