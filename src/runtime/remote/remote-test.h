#pragma once
int ya_remote_init(void);
void ya_remote_stop(void);
/* Only under principal_conn_lock WRITE. No blocking I/O or allocation. */
void ya_remote_session_changed(void);
