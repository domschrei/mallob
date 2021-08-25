
#ifndef DOMPASCH_MALLOB_ASSERT_HPP
#define DOMPASCH_MALLOB_ASSERT_HPP

#if MALLOB_ASSERT
    // Use normal assertions
    #undef NDEBUG
    #define DEBUG
    #include <assert.h>
#else
    // Disable all assertions
    #define assert(expr) (void)0
#endif

#endif
