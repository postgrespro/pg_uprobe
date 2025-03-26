#ifndef JSON_TO_JSONBVALUE_PARSER_H
#define JSON_TO_JSONBVALUE_PARSER_H
#include "postgres.h"
#include "utils/jsonb.h"

extern JsonbValue* jsonToJsonbValue(char* json, size_t len);
#endif