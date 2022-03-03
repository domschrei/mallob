
#ifndef LOGGER_STATIC_VERBOSITY
#define LOGGER_STATIC_VERBOSITY 6
#endif

// Depending on how high the verbosity is, switch the corresponding logger call on or off
#if LOGGER_STATIC_VERBOSITY >= 0
#define LOGGER_LOG_V0(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V0(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 1
#define LOGGER_LOG_V1(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V1(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 2
#define LOGGER_LOG_V2(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V2(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 3
#define LOGGER_LOG_V3(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V3(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 4
#define LOGGER_LOG_V4(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V4(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 5
#define LOGGER_LOG_V5(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V5(LOGGERCALL, VERBSTR, ...) (void)0
#endif
#if LOGGER_STATIC_VERBOSITY >= 6
#define LOGGER_LOG_V6(LOGGERCALL, VERBSTR, ...) LOGGERCALL(VERBSTR, __VA_ARGS__)
#else
#define LOGGER_LOG_V6(LOGGERCALL, VERBSTR, ...) (void)0
#endif

// Relays a generic logger call to the logger macro of the corresponding verbosity
#define LOGGER_LOG(LOGGERCALL, VERB, VERBSTR, ...) LOGGER_LOG_V##VERB(LOGGERCALL, VERBSTR, __VA_ARGS__)

// Log macros for users where the static log function is used
#define LOG(VERB, ...) LOGGER_LOG(Logger::getMainInstance().log, VERB, VERB, __VA_ARGS__)
#define LOG_RETURN_FALSE(...) log_return_false(__VA_ARGS__)
#define LOG_ADD_DEST(VERB, ...) LOGGER_LOG(Logger::getMainInstance().log, VERB, VERB | LOG_ADD_DESTRANK, __VA_ARGS__)
#define LOG_ADD_SRC(VERB, ...) LOGGER_LOG(Logger::getMainInstance().log, VERB, VERB | LOG_ADD_SRCRANK, __VA_ARGS__)
#define LOG_OMIT_PREFIX(VERB, ...) LOGGER_LOG(Logger::getMainInstance().log, VERB, VERB | LOG_NO_PREFIX, __VA_ARGS__)

// Log macros for users where another logger instance is used
#define LOGGER(LOGGER, VERB, ...) LOGGER_LOG(LOGGER.log, VERB, VERB, __VA_ARGS__)
#define LOGGER_ADD_DEST(LOGGER, VERB, ...) LOGGER_LOG(LOGGER.log, VERB, VERB | LOG_ADD_DESTRANK, __VA_ARGS__)
#define LOGGER_ADD_SRC(LOGGER, VERB, ...) LOGGER_LOG(LOGGER.log, VERB, VERB | LOG_ADD_SRCRANK, __VA_ARGS__)
#define LOGGER_OMIT_PREFIX(LOGGER, VERB, ...) LOGGER_LOG(LOGGER.log, VERB, VERB | LOG_NO_PREFIX, __VA_ARGS__)
