CREATE FUNCTION test_ext_backend_model_get()
RETURNS text
AS 'test_ext_backend_model', 'test_ext_backend_model_get'
LANGUAGE C;

CREATE FUNCTION test_ext_backend_model_expect_load_error(text, text)
RETURNS text
AS 'test_ext_backend_model', 'test_ext_backend_model_expect_load_error'
LANGUAGE C STRICT;

CREATE FUNCTION test_ext_backend_model_expect_set_error(text, text)
RETURNS text
AS 'test_ext_backend_model', 'test_ext_backend_model_expect_set_error'
LANGUAGE C STRICT;

SELECT test_ext_backend_model_get();
LOAD 'test_ext_threaded';
LOAD 'plpgsql';
SELECT test_ext_backend_model_expect_load_error('test_ext',
											   'backend model mismatch');
SELECT test_ext_backend_model_expect_load_error('test_ext_bad_backend_model',
											   'invalid backend model');
SELECT test_ext_backend_model_expect_load_error('test_ext_short_magic',
											   'magic block mismatch');
SELECT test_ext_backend_model_expect_set_error('pooled-scheduler',
											  'backend model mismatch');
SELECT test_ext_backend_model_expect_set_error('pooled-protocol-affine',
											  'backend model mismatch');
SELECT test_ext_backend_model_expect_set_error('pooled-protocol-migratable',
											  'backend model mismatch');
