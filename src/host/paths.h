/* SPDX-License-Identifier: LGPL-2.1-or-later */
/** @file paths.h
 * @brief Default locations, XDG-resolved. Shared by daemon and mmctl.
 *
 * Accessors return static strings resolved on first call. Call them from
 * the main thread before starting workers. */

#ifndef MM_PATHS_H
#define MM_PATHS_H

#define MM_APP "mediamaster"

const char *mm_paths_config_dir(void);  /* $XDG_CONFIG_HOME/mediamaster */
const char *mm_paths_config_file(void); /* <config_dir>/config */
const char *mm_paths_state_dir(void);   /* $XDG_STATE_HOME/mediamaster */
const char *mm_paths_log_file(void);    /* <state_dir>/mediamaster.log */
/** $MM_SOCK, else $XDG_RUNTIME_DIR/mediamaster.sock,
 * else /tmp/mediamaster-<uid>.sock */
const char *mm_paths_socket(void);
/** Colon-separated plugin search path: <config_dir>/plugins:MM_PLUGIN_DIR */
const char *mm_paths_plugin_dirs(void);

/** mkdir -p with mode for each created component. @returns 0 on success. */
int mm_mkdir_p(const char *dir, unsigned mode);

#endif
