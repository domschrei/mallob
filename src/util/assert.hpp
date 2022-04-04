
#ifndef DOMPASCH_MALLOB_ASSERT_HPP
#define DOMPASCH_MALLOB_ASSERT_HPP

#if MALLOB_ASSERT
    // Use normal assertions
    #undef NDEBUG
    #define DEBUG
    #include <assert.h>

    #if MALLOB_ASSERT == 2
        #define assert_heavy(expr) assert(expr)
    #else
        #define assert_heavy(expr) (void)0
    #endif
#else
    // Disable all assertions
    #define assert(expr) (void)0
    #define assert_heavy(expr) (void)0
#endif

#endif
