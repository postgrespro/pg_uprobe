-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_uprobe" to load this file. \quit


CREATE FUNCTION set_uprobe(IN func text, IN uprobe_type text, IN is_shared boolean)
RETURNS text
AS 'MODULE_PATHNAME','set_uprobe'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION delete_uprobe(IN func text, IN should_write_stat boolean)
RETURNS void
AS 'MODULE_PATHNAME','delete_uprobe'
LANGUAGE C STABLE STRICT; 

CREATE FUNCTION stat_time_uprobe(IN func text)
RETURNS text
AS 'MODULE_PATHNAME','stat_time_uprobe'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION stat_hist_uprobe( IN func text, IN start double precision, IN stop double precision, IN step double precision, 
    OUT time_range text,
    OUT hist_entry text,
    OUT percent numeric(5,3))
RETURNS SETOF record
AS 'MODULE_PATHNAME','stat_hist_uprobe'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION stat_hist_uprobe( IN func text,
    OUT time_range text,
    OUT hist_entry text,
    OUT percent numeric(5,3))
RETURNS SETOF record
AS 'MODULE_PATHNAME','stat_hist_uprobe_simple'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION list_uprobes( 
    OUT func text,
    OUT uprobe_type text,
    OUT is_shared boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME','list_uprobes'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION dump_uprobe_stat(IN func text, IN should_empty_stat boolean)
RETURNS void
AS 'MODULE_PATHNAME','dump_uprobe_stat'
LANGUAGE C STABLE STRICT;


CREATE FUNCTION start_session_trace()
RETURNS void
AS 'MODULE_PATHNAME','start_session_trace'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION stop_session_trace()
RETURNS void
AS 'MODULE_PATHNAME','stop_session_trace'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION start_session_trace(IN pid INT)
RETURNS void
AS 'MODULE_PATHNAME','start_session_trace_pid'
LANGUAGE C STABLE STRICT;


CREATE FUNCTION stop_session_trace(IN pid INT)
RETURNS void
AS 'MODULE_PATHNAME','stop_session_trace_pid'
LANGUAGE C STABLE STRICT;


CREATE FUNCTION start_lockmanager_trace()
RETURNS void
AS 'MODULE_PATHNAME', 'start_lockmanager_trace'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION stop_lockmanager_trace(IN should_write_stat boolean)
RETURNS void
AS 'MODULE_PATHNAME', 'stop_lockmanager_trace'
LANGUAGE C STABLE STRICT;

CREATE FUNCTION dump_lockmanager_stat(IN should_empty_stat boolean)
RETURNS void
AS 'MODULE_PATHNAME', 'dump_lockmanager_stat'
LANGUAGE C STABLE STRICT;