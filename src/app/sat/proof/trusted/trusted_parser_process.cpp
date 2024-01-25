
#include "trusted_utils.hpp"
#include "trusted_parser.hpp"

int main(int argc, char *argv[]) {
    const char optFormulaInput[] = "-formula-input=";
    const char optParsedFormula[] = "-fifo-parsed-formula=";
    const char* formulaInput;
    const char* fifoParsedFormula;
    for (int i = 0; i < argc; i++) {
        if (TrustedUtils::beginsWith(argv[i], optFormulaInput)) {
            formulaInput = argv[i] + (sizeof(optFormulaInput)-1);
        }
        if (TrustedUtils::beginsWith(argv[i], optParsedFormula)) {
            fifoParsedFormula = argv[i] + (sizeof(optParsedFormula)-1);
        }
    }
    TrustedUtils::log("Using source path", formulaInput);
    TrustedUtils::log("Using output path", fifoParsedFormula);

    // Parse
    FILE* source = fopen(fifoParsedFormula, "w");
    bool ok = TrustedParser(formulaInput, source).parse();
    if (!ok) TrustedUtils::doAbort();
    return 0;
}
