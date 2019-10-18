/*
 * Logger.h
 *
 *  Created on: Mar 9, 2015
 *      Author: balyo
 */

#ifndef LOGGER_H_
#define LOGGER_H_

#include <string>
#include <memory>

#include "default_logging_interface.h"

double getTime();
void setLoggingInterface(std::shared_ptr<LoggingInterface> interface);
void log(int verbosityLevel, const char* fmt, ...);
void exitError(const char* fmt, ...);

#endif /* LOGGER_H_ */
