#include "postgres.h"
#include "common/jsonapi.h"
#include "utils/fmgrprotos.h"
#include "mb/pg_wchar.h"

#include "json_to_jsonbvalue_parser.h"

#if PG_MAJORVERSION_NUM > 15
#define JSON_PARSE_ERROR_TYPE JsonParseErrorType
#define RETURN_SUCCESS JSON_SUCCESS
#define RETURN_ERROR JSON_SEM_ACTION_FAILED
#else
#define JSON_PARSE_ERROR_TYPE void
#define RETURN_SUCCESS /*empty*/
#define RETURN_ERROR /*empty*/

#define IsA(nodeptr,_type_)    (nodeTag(nodeptr) == T_##_type_)

static bool
DirectInputFunctionCallSafe(PGFunction func, char *str,
                            Oid typioparam, int32 typmod,
                            fmNodePtr escontext,
                            Datum *result)
{
    LOCAL_FCINFO(fcinfo, 3);

    if (str == NULL)
    {
        *result = (Datum) 0;    /* just return null result */
        return true;
    }

    InitFunctionCallInfoData(*fcinfo, NULL, 3, InvalidOid, escontext, NULL);

    fcinfo->args[0].value = CStringGetDatum(str);
    fcinfo->args[0].isnull = false;
    fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
    fcinfo->args[1].isnull = false;
    fcinfo->args[2].value = Int32GetDatum(typmod);
    fcinfo->args[2].isnull = false;

    *result = (*func) (fcinfo);

    /* Otherwise, shouldn't get null result */
    if (fcinfo->isnull)
        elog(ERROR, "input function %p returned NULL", (void *) func);

    return true;
}

#endif

typedef struct JsonbInState
{
    JsonbParseState *parseState;
    JsonbValue *res;
} JsonbInState;

static JSON_PARSE_ERROR_TYPE jsonb_in_object_start(void *pstate);
static JSON_PARSE_ERROR_TYPE jsonb_in_object_end(void *pstate);
static JSON_PARSE_ERROR_TYPE jsonb_in_array_start(void *pstate);
static JSON_PARSE_ERROR_TYPE jsonb_in_array_end(void *pstate);
static JSON_PARSE_ERROR_TYPE jsonb_in_object_field_start(void *pstate, char *fname, bool isnull);
static JSON_PARSE_ERROR_TYPE jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype);

JsonbValue*
jsonToJsonbValue(char* json, size_t len)
{
    JsonLexContext* lex;
    JsonbInState state;
    JsonSemAction sem;
    JsonbValue* result;

    memset(&state, 0, sizeof(state));
    memset(&sem, 0, sizeof(sem));
#if PG_MAJORVERSION_NUM > 16
    lex = palloc0(sizeof(JsonLexContext));
    makeJsonLexContextCstringLen(lex, json, len, GetDatabaseEncoding(), true);
#else
    lex = makeJsonLexContextCstringLen(json, len, GetDatabaseEncoding(), true);
#endif
    sem.semstate = (void *) &state;

    sem.object_start = jsonb_in_object_start;
    sem.array_start = jsonb_in_array_start;
    sem.object_end = jsonb_in_object_end;
    sem.array_end = jsonb_in_array_end;
    sem.scalar = jsonb_in_scalar;
    sem.object_field_start = jsonb_in_object_field_start;

    if (pg_parse_json(lex, &sem) != JSON_SUCCESS)
    {
        result = NULL;
    }
    /* after parsing, the item member has the composed jsonb structure */
    result = state.res;
    pfree(lex);

    return result;
}


static JSON_PARSE_ERROR_TYPE
jsonb_in_object_start(void *pstate)
{
    JsonbInState *_state = (JsonbInState *) pstate;

    _state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_OBJECT, NULL);

    return RETURN_SUCCESS;
}

static JSON_PARSE_ERROR_TYPE
jsonb_in_object_end(void *pstate)
{
    JsonbInState *_state = (JsonbInState *) pstate;

    _state->res = pushJsonbValue(&_state->parseState, WJB_END_OBJECT, NULL);

    return RETURN_SUCCESS;
}

static JSON_PARSE_ERROR_TYPE
jsonb_in_array_start(void *pstate)
{
    JsonbInState *_state = (JsonbInState *) pstate;

    _state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, NULL);

    return RETURN_SUCCESS;
}

static JSON_PARSE_ERROR_TYPE
jsonb_in_array_end(void *pstate)
{
    JsonbInState *_state = (JsonbInState *) pstate;

    _state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);

    return RETURN_SUCCESS;
}

static JSON_PARSE_ERROR_TYPE
jsonb_in_object_field_start(void *pstate, char *fname, bool isnull)
{
    JsonbInState *_state = (JsonbInState *) pstate;
    JsonbValue	v;

    Assert(fname != NULL);
    v.type = jbvString;
    v.val.string.len = strlen(fname);
    v.val.string.val = fname;

    _state->res = pushJsonbValue(&_state->parseState, WJB_KEY, &v);

    return RETURN_SUCCESS;
}

/*
 * For jsonb we always want the de-escaped value - that's what's in token
 */
static JSON_PARSE_ERROR_TYPE
jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype)
{
    JsonbInState *_state = (JsonbInState *) pstate;
    JsonbValue	v;
    Datum		numd;

    switch (tokentype)
    {

        case JSON_TOKEN_STRING:
            Assert(token != NULL);
            v.type = jbvString;
            v.val.string.len = strlen(token);
            v.val.string.val = token;
            break;
        case JSON_TOKEN_NUMBER:

            /*
             * No need to check size of numeric values, because maximum
             * numeric size is well below the JsonbValue restriction
             */
            Assert(token != NULL);
            v.type = jbvNumeric;
            if (!DirectInputFunctionCallSafe(numeric_in, token,
                                             InvalidOid, -1,
                                             NULL,
                                             &numd))
                return RETURN_ERROR;
            v.val.numeric = DatumGetNumeric(numd);
            break;
        case JSON_TOKEN_TRUE:
            v.type = jbvBool;
            v.val.boolean = true;
            break;
        case JSON_TOKEN_FALSE:
            v.type = jbvBool;
            v.val.boolean = false;
            break;
        case JSON_TOKEN_NULL:
            v.type = jbvNull;
            break;
        default:
            /* should not be possible */
            elog(ERROR, "invalid json token type");
            break;
    }

    if (_state->parseState == NULL)
    {
        /* single scalar */
        JsonbValue	va;

        va.type = jbvArray;
        va.val.array.rawScalar = true;
        va.val.array.nElems = 1;

        _state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, &va);
        _state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
        _state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);
    }
    else
    {
        JsonbValue *o = &_state->parseState->contVal;

        switch (o->type)
        {
            case jbvArray:
                _state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
                break;
            case jbvObject:
                _state->res = pushJsonbValue(&_state->parseState, WJB_VALUE, &v);
                break;
            default:
                elog(ERROR, "unexpected parent of nested structure");
        }
    }

    return RETURN_SUCCESS;
}