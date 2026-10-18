#ifndef API_H
#define API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The command layer: each operation on the machine (status, keys, turbo, discs, the LEDs, a
// program's commands, ...) implemented once, by name, for every transport. The USB channel
// (frontend.c: "!<name> <args>") and a board's HTTP server ("/api/<name>") call api_call; the web
// page (web/index.html) and tools/presto.py use the same names.
//
// A request carries its arguments (USB: the rest of the line; HTTP: the body) and asks for one
// of two replies: JSON (HTTP, and USB "!api <name> [args]"), written into the request's buffer,
// or the USB channel's own text line(s), printed (as tools/presto.py has always read them).
// Both come from the same handler, side by side. Everything runs on core 0, between frames.

typedef struct {
    const char *args;   // the arguments ("" if none)
    const char *query;  // HTTP's query string ("" if none)
    bool json;          // reply in JSON (into out), else print the text reply
    char *out;          // the JSON reply
    size_t size;
    int code;           // and its HTTP status: 200 unless a handler says otherwise
} api_req_t;

// what a command is
typedef struct {
    const char *name;       // as the USB channel spells it, and "/api/<name>" (unless http is set)
    const char *http;       // its HTTP path if not "/api/<name>"; NULL
    bool post;              // HTTP: POST (it changes something), else GET
    void (*fn)(api_req_t *r);
} api_cmd_t;

// the command called name (an HTTP path if it starts with '/'), or NULL
const api_cmd_t *api_find(const char *name);
const api_cmd_t *api_find_http(const char *path, bool post);
// carry out a command: false if there's no such command
bool api_call(const char *name, api_req_t *r);
void api_run(const api_cmd_t *c, api_req_t *r);

// replies: JSON into r->out (with an HTTP status), or nothing (text replies are printed)
void api_reply(api_req_t *r, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void api_error(api_req_t *r, int code, const char *why);
// a JSON string, quoted and escaped, into out (its length)
int api_json_str(char *out, size_t size, const char *s);

// keys from the web page (and "!keys"), by matrix code, held a few frames each: let out between
// frames (frontend_frame)
void api_frame(void);

#ifdef __cplusplus
}
#endif

#endif
