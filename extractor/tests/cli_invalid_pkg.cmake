if(NOT DEFINED PKG_EXTRACTOR OR NOT DEFINED TEST_DIRECTORY)
	message(FATAL_ERROR "PKG_EXTRACTOR and TEST_DIRECTORY must be provided")
endif()

set(missing_pkg "${TEST_DIRECTORY}/missing package.pkg")
set(empty_pkg "${TEST_DIRECTORY}/empty package.pkg")
set(truncated_pkg "${TEST_DIRECTORY}/truncated package.pkg")
file(REMOVE "${missing_pkg}")
file(WRITE "${empty_pkg}" "")
string(ASCII 127 67 78 84 pkg_magic)
file(WRITE "${truncated_pkg}" "${pkg_magic}")

execute_process(
	COMMAND "${PKG_EXTRACTOR}" "${missing_pkg}"
	RESULT_VARIABLE missing_result
	OUTPUT_VARIABLE missing_stdout
	ERROR_VARIABLE missing_stderr)
execute_process(
	COMMAND "${PKG_EXTRACTOR}" "${empty_pkg}"
	RESULT_VARIABLE empty_result
	OUTPUT_VARIABLE empty_stdout
	ERROR_VARIABLE empty_stderr)
execute_process(
	COMMAND "${PKG_EXTRACTOR}" "${truncated_pkg}"
	RESULT_VARIABLE truncated_result
	OUTPUT_VARIABLE truncated_stdout
	ERROR_VARIABLE truncated_stderr)

file(REMOVE "${empty_pkg}" "${truncated_pkg}")

if(NOT missing_result STREQUAL "1")
	message(FATAL_ERROR "Missing package exited with '${missing_result}', expected 1. stdout: ${missing_stdout} stderr: ${missing_stderr}")
endif()
if(NOT missing_stdout MATCHES "doesn't appear to be a valid PKG file")
	message(FATAL_ERROR "Missing package stdout did not report an invalid PKG: ${missing_stdout}")
endif()
if(NOT empty_result STREQUAL "1")
	message(FATAL_ERROR "Empty package exited with '${empty_result}', expected 1. stdout: ${empty_stdout} stderr: ${empty_stderr}")
endif()
if(NOT empty_stdout MATCHES "doesn't appear to be a valid PKG file")
	message(FATAL_ERROR "Empty package stdout did not report an invalid PKG: ${empty_stdout}")
endif()
if(NOT truncated_result STREQUAL "1")
	message(FATAL_ERROR "Truncated package exited with '${truncated_result}', expected 1. stdout: ${truncated_stdout} stderr: ${truncated_stderr}")
endif()
if(NOT truncated_stdout MATCHES "PKG header is truncated")
	message(FATAL_ERROR "Truncated package stdout did not report its invalid header: ${truncated_stdout}")
endif()
