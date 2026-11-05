#ifndef SG_AI_H
#define SG_AI_H

/* stategate ai fill | work | connect | check: Claude Code over the stategate
 * MCP server. argv0 finds this binary for the server command. */
int cmd_ai(int argc, char **argv, const char *argv0);

/* stategate mcp config: the MCP server entry for this project, as JSON for
 * any MCP client. */
int cmd_mcp_config(const char *argv0);

#endif
