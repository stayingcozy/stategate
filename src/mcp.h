#ifndef SG_MCP_H
#define SG_MCP_H

/* Serves the AI steps as MCP tools over stdin/stdout until stdin closes.
 * argv0 is used to re-run this binary when /proc/self/exe is unavailable. */
int cmd_mcp(const char *model_path, const char *argv0);

#endif
