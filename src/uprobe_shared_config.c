#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/varlena.h"
#include "utils/jsonb.h"

#include "uprobe_shared_config.h"

#define CONFIG_PATH "./pg_uprobe/pg_uprobe_conf.jsonb"


static void WriteJsonbToConfigFile(Jsonb *j);
static char *ReadConfigToString(long *resultSize);

static void
WriteJsonbToConfigFile(Jsonb *j)
{
	FILE	   *config = fopen(CONFIG_PATH, "w");
	uint32		size = VARSIZE_4B(j);

	if (config == NULL)
	{
		elog(ERROR, "can't open config base/" CONFIG_PATH " file for writing");
	}
	if (fwrite(j, 1, size, config) != size)
	{
		elog(ERROR, "can't write full config base/" CONFIG_PATH);
	}
	fclose(config);
}

static char *
ReadConfigToString(long *resultSize)
{
	long		fsize;
	char	   *string;
	FILE	   *f = fopen(CONFIG_PATH, "rb");

	if (f == NULL)
		return NULL;

	fseek(f, 0, SEEK_END);
	fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	string = palloc(fsize);
	if (fread(string, fsize, 1, f) != 1)
	{
		pfree(string);
		return NULL;
	}

	fclose(f);
	if (resultSize)
		*resultSize = fsize;

	return string;
}

/* should be called only in transactions */
void
PGUprobeSaveInSharedConfig(char *func, char *type)
{

	text	   *config;
	JsonbParseState *parse_state = NULL;
	JsonbValue	json_value_config;
	JsonbPair	pair;
	Jsonb	   *result_config;
	JsonbIteratorToken iter_token;
	JsonbIterator *json_iterator;
	JsonbValue	v;

	PG_TRY();
	{
		config = DatumGetTextP(DirectFunctionCall1(pg_read_binary_file_all, CStringGetTextDatum(CONFIG_PATH)));
	}
	PG_CATCH();
	{
		elog(LOG, "can't read uprobe config will create new");
		config = palloc(sizeof(text));
		SET_VARSIZE_4B(config, VARHDRSZ);
	}
	PG_END_TRY();

	/* <func>:<type> */
	pair.key.type = jbvString;
	pair.key.val.string.len = (int) strlen(func) + 1;
	pair.key.val.string.val = func;
	pair.value.type = jbvString;
	pair.value.val.string.len = (int) strlen(type) + 1;
	pair.value.val.string.val = type;
	if (VARSIZE_ANY_EXHDR(config) != 0)
	{
		JsonbToJsonbValue((Jsonb *) VARDATA_ANY(config), &json_value_config);
		json_iterator = JsonbIteratorInit(json_value_config.val.binary.data);
		iter_token = JsonbIteratorNext(&json_iterator, &v, false);
		Assert(iter_token == WJB_BEGIN_OBJECT);

		while (iter_token != WJB_END_OBJECT)
		{

			pushJsonbValue(&parse_state, iter_token,
						   iter_token < WJB_BEGIN_ARRAY ||
						   (iter_token == WJB_BEGIN_ARRAY &&
							v.val.array.rawScalar) ? &v : NULL);
			iter_token = JsonbIteratorNext(&json_iterator, &v, false);
		}
		/* we do it to end iteration correctly */
		iter_token = JsonbIteratorNext(&json_iterator, &v, false);
		Assert(iter_token == WJB_DONE);
	}
	else
	{
		pushJsonbValue(&parse_state, WJB_BEGIN_OBJECT, NULL);
	}


	pushJsonbValue(&parse_state, WJB_KEY, &pair.key);
	pushJsonbValue(&parse_state, WJB_VALUE, &pair.value);
	result_config = JsonbValueToJsonb(pushJsonbValue(&parse_state, WJB_END_OBJECT, NULL));

	WriteJsonbToConfigFile(result_config);
	pfree(result_config);
}


void
PGUprobeLoadFromSharedConfig(LoadFromConfigApplyFunc applyFunc)
{
	char	   *config = NULL;
	long		configSize;
	JsonbValue	json_value_config;
	JsonbIteratorToken iter_token;
	JsonbValue	k;
	JsonbValue	v;
	JsonbIterator *json_iterator;

	config = ReadConfigToString(&configSize);

	if (configSize == 0 || config == NULL)
	{
		elog(LOG, "can't load from empty config");
		return;
	}

	JsonbToJsonbValue((Jsonb *) config, &json_value_config);
	json_iterator = JsonbIteratorInit(json_value_config.val.binary.data);
	iter_token = JsonbIteratorNext(&json_iterator, &v, false);

	Assert(iter_token == WJB_BEGIN_OBJECT);
	while (iter_token != WJB_END_OBJECT)
	{
		iter_token = JsonbIteratorNext(&json_iterator, &k, false);
		if (iter_token == WJB_END_OBJECT)
			break;

		Assert(iter_token == WJB_KEY);
		Assert(k.type == jbvString);

		iter_token = JsonbIteratorNext(&json_iterator, &v, false);
		Assert(iter_token == WJB_VALUE);
		Assert(v.type == jbvString);

		applyFunc(k.val.string.val, v.val.string.val);
	}
	/* we do it to end iteration correctly */
	iter_token = JsonbIteratorNext(&json_iterator, &v, false);
	Assert(iter_token == WJB_DONE);
	pfree(config);
}


void
PGUprobeDeleteFromSharedConfig(const char *func)
{
	char	   *config = NULL;
	long		configSize;
	JsonbParseState *parse_state = NULL;
	JsonbValue	json_value_config;
	JsonbIteratorToken iter_token;
	JsonbValue	v;
	JsonbIterator *json_iterator;
	bool		skip_next_value = false;
	Jsonb	   *result_config;

	config = ReadConfigToString(&configSize);

	if (configSize == 0 || config == NULL)
	{
		elog(LOG, "can't delete from empty config");
		return;
	}

	JsonbToJsonbValue((Jsonb *) config, &json_value_config);
	json_iterator = JsonbIteratorInit(json_value_config.val.binary.data);
	iter_token = JsonbIteratorNext(&json_iterator, &v, false);
	Assert(iter_token == WJB_BEGIN_OBJECT);

	while (iter_token != WJB_END_OBJECT)
	{
		if (skip_next_value && iter_token == WJB_VALUE)
		{
			skip_next_value = false;
			goto next_iteration;
		}
		if (iter_token == WJB_KEY)
		{
			Assert(v.type == jbvString);
			if (!strcmp(v.val.string.val, func))
			{
				skip_next_value = true;
				goto next_iteration;
			}
		}


		pushJsonbValue(&parse_state, iter_token,
					   iter_token < WJB_BEGIN_ARRAY ||
					   (iter_token == WJB_BEGIN_ARRAY &&
						v.val.array.rawScalar) ? &v : NULL);
next_iteration:
		iter_token = JsonbIteratorNext(&json_iterator, &v, false);
	}

	/* we do it to end iteration correctly */
	iter_token = JsonbIteratorNext(&json_iterator, &v, false);
	Assert(iter_token == WJB_DONE);

	result_config = JsonbValueToJsonb(pushJsonbValue(&parse_state, WJB_END_OBJECT, NULL));
	WriteJsonbToConfigFile(result_config);
	pfree(result_config);
	pfree(config);
}
