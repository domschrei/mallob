
#pragma once

#include <charconv>

#include "data/serializable.hpp"
#include "util/assert.hpp"
#include "util/compression.hpp"
#include "util/logger.hpp"
#include "integer_rule.hpp"

class TheorySpecification : public Serializable {

private:
    std::vector<IntegerRule> ruleset;

public:
    TheorySpecification() {}
    TheorySpecification(const std::string& spec) {
        parse(spec);
    }
    TheorySpecification(std::initializer_list<IntegerRule>&& rules) : ruleset(std::move(rules)) {}

    const std::vector<IntegerRule>& getRuleset() const {
        return ruleset;
    }

    std::string toStr() const {
        std::string out;
        for (auto& rule : ruleset) out += rule.toStr() + "&";
        return out.substr(0, out.size()-1);
    }

    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> out;
        for (const auto& rule : ruleset) {
            auto rulePacked = rule.serialize();
            uint8_t data[5];
            int bytelen = toVariableBytelength<int>(rulePacked.size(), data);
            out.insert(out.end(), data, data+bytelen);
            out.insert(out.end(), rulePacked.begin(), rulePacked.end());
        }
        return out;
    }
    virtual TheorySpecification& deserialize(const std::vector<uint8_t>& packed) override {
        int pos = 0;
        while (pos < packed.size()) {
            int bytelen;
            int ruleSize = fromVariableBytelength<int>(packed.data() + pos, bytelen);
            pos += bytelen;
            std::vector<uint8_t> rulePacked(packed.data()+pos, packed.data()+pos+ruleSize);
            IntegerRule rule;
            rule.deserialize(rulePacked);
            ruleset.push_back(std::move(rule));
            pos += ruleSize;
        }
        assert(pos == packed.size());
        return *this;
    }

private:
    void parse(const std::string& specification) {
        std::string fullspec = specification;
        //LOG(V2_INFO, "PARSE_RULE \"%s\"\n", fullspec.c_str());

        // remove all spaces
        fullspec.erase(std::remove_if(fullspec.begin(), fullspec.end(), ::isspace), fullspec.end());

        while (!fullspec.empty()) {
            size_t end = fullspec.find_first_of(';', 0);
            if (end == std::string::npos) end = fullspec.size();
            std::string spec = fullspec.substr(0, end);
            fullspec = end+1 >= spec.size() ? "" : fullspec.substr(end+1);

            if (spec.size() > 3 && spec.substr(0, 3) == "min") {
                ruleset.push_back({IntegerRule::MINIMIZE, parseIntTerm(spec.substr(3))});
            }
            if (spec.size() > 3 && spec.substr(0, 3) == "max") {
                ruleset.push_back({IntegerRule::MAXIMIZE, parseIntTerm(spec.substr(3))});
            }
            if (spec.size() > 3 && spec.substr(0, 3) == "inv") {
                for (size_t pos = 0; pos < spec.size(); pos++) {
                    if (spec[pos] == '=' || spec[pos] == '<' || spec[pos] == '>' || spec[pos] == '!') {
                        std::string op(spec.data()+pos, 1);
                        int size = 1;
                        if (spec[pos] == '<' && spec[pos+1] == '=') {op = "<="; size = 2;}
                        if (spec[pos] == '>' && spec[pos+1] == '=') {op = ">="; size = 2;}
                        if (spec[pos] == '!' && spec[pos+1] == '=') {op = "!="; size = 2;}
                        ruleset.push_back({IntegerRule::INVARIANT, op,
                            parseIntTerm(spec.substr(3, pos-3)),
                            parseIntTerm(spec.substr(pos+size))
                        });
                        break;
                    }
                }
            }
        }
    }

    IntegerTerm parseIntTerm(const std::string& s) {

        struct StackElem {
            int specBegin; int specEnd; IntegerTerm res; int returnIndex;
            StackElem() {}
            StackElem(int specBegin, int specEnd, int returnIndex) :
                specBegin(specBegin), specEnd(specEnd), res(), returnIndex(returnIndex) {}
        };
        std::vector<StackElem> stack(1, {0, (int)s.size(), -1});

        std::vector<std::pair<char, IntegerTerm::Type>> opsByPrecedence;
        if (s.find("+") != std::string::npos) opsByPrecedence.push_back({'+', IntegerTerm::ADD});
        if (s.find("-") != std::string::npos) opsByPrecedence.push_back({'-', IntegerTerm::SUBTRACT});
        if (s.find("*") != std::string::npos) opsByPrecedence.push_back({'*', IntegerTerm::MULTIPLY});
        if (s.find("/") != std::string::npos) opsByPrecedence.push_back({'/', IntegerTerm::DIVIDE});
        if (s.find("%") != std::string::npos) opsByPrecedence.push_back({'%', IntegerTerm::MODULO});
        if (s.find("^") != std::string::npos) opsByPrecedence.push_back({'^', IntegerTerm::POW});

        const char* spec = s.data();
        while (!stack.empty()) {
            StackElem& elem = stack.back();
            int stackPos = stack.size()-1;
            int specBegin = elem.specBegin;
            int specEnd = elem.specEnd;
            int specSize = specEnd - specBegin;
            //LOG(V2_INFO, "STACK_ELEM \"%s\"\n", s.substr(specBegin, specEnd-specBegin).c_str());

            // returning to a previously visited element? -> done
            if (elem.res.type != IntegerTerm::NONE) {
                //LOG(V2_INFO, "  return\n");
                if (elem.returnIndex == -1) {
                    // completely done
                    assert(stackPos == 0);
                    break;
                }
                stack[elem.returnIndex].res.addChildAndTryFlatten(std::move(elem.res));
                stack.pop_back();
                continue;
            }

            if (specSize == 0) {
                // nothing to do
                //LOG(V2_INFO, "  empty\n");
                stack.pop_back();
                continue;
            }

            // top-level parentheses
            if (spec[specBegin] == '(' && spec[specEnd-1] == ')') {
                assert(specSize >= 2);
                size_t pos = specBegin;
                int bracketCount = 0;
                while (true) {
                    if (spec[pos] == '(') bracketCount++;
                    if (spec[pos] == ')') bracketCount--;
                    if (bracketCount==0 || pos==specEnd-1) break;
                    pos++;
                }
                assert(bracketCount == 0);
                if (pos == specEnd-1) {
                    // remove parentheses
                    //LOG(V2_INFO, "  parenthesis\n");
                    elem.specBegin++;
                    elem.specEnd--;
                    continue;
                }
            }

            // top-level operator
            bool pushed {false};
            for (auto [op, type] : opsByPrecedence) {
                int pos = specEnd;
                int bracketCount = 0;
                while (pos > specBegin) {
                    pos--;
                    if (spec[pos] == '(') bracketCount++;
                    if (spec[pos] == ')') bracketCount--;
                    // top-level operation outside of any brackets?
                    if (bracketCount==0 && spec[pos] == op) {
                        // ignore minus-as-a-sign token
                        if (op=='-' && (pos==elem.specBegin || spec[pos-1] == '(' || isOp(spec[pos-1]))) continue;
                        //LOG(V2_INFO, "  op pos=%lu \"%c\"\n", pos, op);
                        int parentIndex = elem.returnIndex;
                        if ((type == IntegerTerm::ADD || type == IntegerTerm::MULTIPLY)
                            && parentIndex >= 0 && stack[parentIndex].res.type == type) {
                            // flatten binary operation into n-ary operation
                            // right child element (overwrite current element)
                            elem.specBegin = pos+1;
                            // left child element
                            stack.push_back({specBegin, pos, parentIndex});
                        } else {
                            // can't be flattened: push two new child elements
                            elem.res.setType(type);
                            stack.push_back({pos+1, specEnd, stackPos});
                            stack.push_back({specBegin, pos, stackPos});
                        }
                        pushed = true;
                        break;
                    }
                }
                if (pushed) break;
            }
            if (pushed) continue;
            // no top-level operators: atomic level reached

            // negation
            if (spec[specBegin] == '-') {
                //LOG(V2_INFO, "  negate\n");
                elem.res.setType(IntegerTerm::NEGATE);
                stack.push_back({specBegin+1, specEnd, stackPos});
                continue;
            }

            // literal
            if (spec[specBegin] == '.' || spec[specBegin] == '!') {
                //LOG(V2_INFO, "  lit\n");
                elem.res.setType(IntegerTerm::LITERAL);
                std::from_chars(spec+specBegin+1, spec+specEnd, elem.res.inner());
                if (spec[specBegin] == '!') elem.res.inner() *= -1;
                continue; // will be "returned" right away
            }

            // constant
            //LOG(V2_INFO, "  const\n");
            elem.res.setType(IntegerTerm::CONSTANT);
            std::from_chars(spec+specBegin, spec+specEnd, elem.res.inner());
            continue; // will be "returned" right away
        }
        assert(stack.size() == 1);
        return std::move(stack[0].res);
    }

    bool isOp(char c) {
        return c=='+' || c=='-' || c=='*' || c=='/' || c=='%' || c=='^';
    }

    IntegerTerm::Type getOp(char c) {
        if (c =='+') return IntegerTerm::ADD;
        if (c =='-') return IntegerTerm::SUBTRACT;
        if (c =='*') return IntegerTerm::MULTIPLY;
        if (c =='/') return IntegerTerm::DIVIDE;
        if (c =='%') return IntegerTerm::MODULO;
        if (c =='^') return IntegerTerm::POW;
        return IntegerTerm::NONE;
    }
};
