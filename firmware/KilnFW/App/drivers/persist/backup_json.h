#ifndef BACKUP_JSON_H
#define BACKUP_JSON_H

// Minimal, purpose-built JSON reader for backup_import.c's exact fixed
// schema -- split out of backup_http.c (2026-09-04, ROADMAP.md M15's
// 1500-line item) with backup_http_internal.h; see that header's own
// comment for the full split map. MOVE-ONLY: these functions were `static`
// in the original single file, unchanged apart from the `backup_json_`
// prefix (added for the split, not because any collision was found --
// see backup_http_internal.h's comment on the audit convention this repo
// follows).
//
// There is no cJSON (or any other JSON library) anywhere in this codebase --
// every existing GET handler hand-builds JSON with snprintf, and every
// existing POST handler parses application/x-www-form-urlencoded via
// http_form.h. This import is the one place a POST body is JSON rather than
// form-encoded. Rather than take on a general-purpose JSON library
// dependency for one upload endpoint, this is a small, purpose-built reader
// for EXACTLY backup_import.c's own fixed schema -- it does not aim to parse
// arbitrary JSON correctly (no unicode escapes, no scientific-notation edge
// cases beyond what strtod already handles, no duplicate-key-wins-last
// semantics beyond "first match found wins"). Deliberately iterative, not
// recursive, when skipping nested {..}/[..] (backup_json_skip_value() below
// uses a depth counter, not a call stack) -- a malicious deeply-nested body
// costs more loop iterations, never more stack, which matters on a board
// that once had a real stack-overflow boot loop (commit aee6171).

#include <stddef.h>
#include <stdbool.h>

const char *backup_json_skip_ws(const char *p);

// Advances past one JSON value (string/number/object/array/true/false/null)
// starting at *p (leading whitespace tolerated), returning a pointer just
// past it. Malformed input still advances (at least one byte) so a caller
// scanning for the next field/element cannot spin forever on garbage.
const char *backup_json_skip_value(const char *p);

// obj must point at (or before, with only whitespace between) a '{'. Returns
// a pointer to the start of key's value if found at this object's top level
// (does not descend into nested objects/arrays looking for the same key
// elsewhere), or NULL if the key is absent or obj is not a well-formed
// object.
const char *backup_json_obj_find(const char *obj, const char *key);

// arr must point at (or before, with only whitespace between) a '['. Returns
// a pointer to the first element's value, or NULL if the array is empty,
// absent, or malformed.
const char *backup_json_arr_first(const char *arr);

// elem points at one array element's value (as returned by
// backup_json_arr_first() or a previous call to this function). Returns a
// pointer to the next element, or NULL once the array's closing ']' is
// reached.
const char *backup_json_arr_next(const char *elem);

bool backup_json_field_num(const char *obj, const char *key, double *out);

// Parses an OPTIONAL bounded numeric field: absent is not an error (*out_has
// is set false, *out untouched), present-but-out-of-range or malformed IS an
// error. Used by backup_import_apply()'s zone-tuning pass 1b for every
// version-2+ field, all of which are optional the same way model_k_dc/
// tc_type already were at version 1 -- a version-1 export simply never has
// these keys, and this is what lets it still import cleanly under a later
// version's reader (see backup_http_internal.h's BACKUP_FORMAT_VERSION
// comment in backup_http.c).
bool backup_json_field_opt_num(const char *obj, const char *key, double min, double max, double *out,
                               bool *out_has, const char *field_desc, char *err_msg, size_t err_cap,
                               unsigned entry_idx);

bool backup_json_field_str(const char *obj, const char *key, char *out, size_t cap);

// Strict whole-document check, run BEFORE any field scanner touches a body (HTTP fuzz F4/F5/F6): `doc` must
// be exactly one complete, well-formed JSON object (RFC 8259 grammar, nesting depth <= 32, no trailing
// bytes other than whitespace) and must not repeat any key at its top level. Iterative, constant stack.
// Returns true when valid; otherwise false with a short reason in err (may be NULL).
bool backup_json_validate_document(const char *doc, char *err, size_t err_cap);

// True when the top-level object `obj` has `key` and its value is not a JSON array (F5). False when the key
// is absent or the value is an array.
bool backup_json_key_present_not_array(const char *obj, const char *key);

#endif // BACKUP_JSON_H
