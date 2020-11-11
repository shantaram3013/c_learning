#include "editor.h"
#include "termutils.h"

void init_editor(struct editor_state *state) {
    if (state->filename) {
        free(state->filename);
    }
    if (state->row) {
        free(state->row);
    }
    state->cx = 0;
    state->cy = 0;
    state->n_rows = 0;
    state->row_offset = 0;
    state->column_offset = 0;
    state->rx = 0;
    state->row = NULL;
    state->filename = NULL;
    state->status_msg[0] = '\0';
    state->status_time = 0;
    state->dirty = 0;
    if (get_window_size(state) == -1) die("get_window_size", state);
    state->cols += 1;
    state->rows -= STATUSLINE_COUNT;
}

void editor_open_file(struct editor_state *state, const char *filename) {
    free(state->filename);
    state->filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen", state);
    else printf("opened %s for reading\n", state->filename);
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        editor_insert_row(state, line, line_len, state->n_rows);
    }
    free(line);
    fclose(fp);
    state->dirty = 0;
}

void editor_save_file(struct editor_state *state) {
    if (state->filename == NULL) {
        editor_set_status(state, "Enter filename to save to:");
        state->filename = e_get_prompt_response(state);
        if (state->filename == NULL) {
            editor_set_status(state, "Save cancelled by user.");
            return;
        }
    }
    int len;
    char *buf = editor_rows_to_string(state, &len);
    char *p = buf + len;
    char *filename = strdup(state->filename);
    size_t cx = state->cx;
    size_t cy = state->cy;

    while (*--p == '\n') {
        *p = '\0';
        len -= 1;
    }
    len += 1;
    *++p = '\n';
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                init_editor(state);
                editor_open_file(state, filename);
                state->cx = cx;
                state->cy = cy;
                if (cy > state->n_rows) {
                    state->cy -= (cy - state->n_rows);
                }
                editor_set_status(state, "wrote %d bytes to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(filename);
    free(buf);
}

void e_draw_message_bar(struct editor_state *state, struct abuf *ab) {
    ab_append(ab, "\33[1m", 4);
    ab_append(ab, "\x1b[K", 4);
    int msg_len = strlen(state->status_msg);
    if (msg_len > (int) state->cols) msg_len = state->cols;
    if (msg_len && time(NULL) - state->status_time < STATUS_TIMEOUT)
        ab_append(ab, state->status_msg, msg_len);
    ab_append(ab, "\33[1m", 5);
    ab_append(ab, "\r\n", 2);
}

void editor_insert_row(struct editor_state *state, char *s, size_t len, size_t at) {
    if (at > state->n_rows)
        return;
    state->row = realloc(state->row, sizeof(e_row) * (state->n_rows + 1));
    memmove(&state->row[at + 1], &state->row[at], sizeof(e_row) * (state->n_rows - at));
    state->row[at].size = len;
    state->row[at].chars = malloc(len + 1);
    memcpy(state->row[at].chars, s, len);
    state->row[at].chars[len] = '\0';
    state->row[at].render_size = 0;
    state->row[at].render = NULL;
    editor_update_row(&state->row[at]);
    state->n_rows++;
    state->dirty++;
}
void editor_draw_status(struct editor_state *state, struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 5);
    char status[80], rstatus[80];
    size_t len = snprintf(status, sizeof(status), "%.20s - %zu lines %s", state->filename ? state->filename : "[No Name]", state->n_rows,
                          state->dirty ? "(modified)" : "");
    size_t rlen = snprintf(rstatus, sizeof(rstatus), "cy %zu: cx %zu", state->cy, state->cx);
    if (len > state->cols)
        len = state->cols;
    ab_append(ab, status, len);
    while (len < state->cols - 1) {
        if (state->cols - len - 1 == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        }
        else {
            ab_append(ab, " ", 2);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 4);
}

void editor_set_status(struct editor_state *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->status_msg, sizeof(state->status_msg) - 2, fmt, ap);
    va_end(ap);
    state->status_time = time(NULL);
}

void editor_draw_rows(struct editor_state *state, struct abuf *ab) {
    for (size_t y = 0; y < state->rows; y++) {
        size_t file_row = y +state->row_offset;
        if (file_row >= state->n_rows || (state->n_rows == 0 && state->filename == NULL)) {
            ab_append(ab, "~", 1);
        }
        else {
            int len = state->row[file_row].render_size - state->column_offset;
            if (len < 0) len = 0;
            if (len > (int) state->cols) len = state->cols;
            ab_append(ab, &state->row[file_row].render[state->column_offset], len);
        }
        ab_append(ab, "\x1b[K", 4);
        if (y < state->rows) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

void editor_scroll(struct editor_state *state) {
    state->rx = state->cx;
    state->rx = 0;
    if (state->cy < state->n_rows) {
        state->rx = e_row_cx_to_rx(&state->row[state->cy], state->cx);
    }
    if (state->cy < state->row_offset) {
        state->row_offset = state->cy;
    }
    if (state->cy >= state->row_offset + state->rows) {
        state->row_offset = state->cy - state->rows + 1;
    }
    if (state->rx < state->column_offset) {
        state->column_offset = state->rx;
    }
    if (state->rx >= state->column_offset + state->cols) {
        state->column_offset = state->rx - state->cols + 1;
    }
}

void editor_refresh_screen(struct editor_state* state) {
    editor_scroll(state);

    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[?25l", 6); // hide cursor
    ab_append(&ab, "\x1b[H", 3);
    editor_draw_rows(state, &ab);

    e_draw_message_bar(state, &ab);
    editor_draw_status(state, &ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", (state->cy - state->row_offset) + 1, state->rx + 1);
    ab_append(&ab, buf, strlen(buf));
    ab_append(&ab, "\x1b[?25h", 6); // show cursor
    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

int editor_read_key(struct editor_state *state) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read", state);
  }
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '5': return PAGE_UP;
                    case '3': return DEL;
                    case '6': return PAGE_DOWN;
                    case '1': return HOME;
                    case '4': return END;
                    case '7': return HOME;
                    case '8': return END;
                }
        }
    } else {
      switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME;
        case 'F': return END;
      }
    }    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME;
        case 'F': return END;
      }
  }
    return '\x1b';
  } else {
    return c;
  }
}

void editor_process_keypress(struct editor_state *state) {
    int c = editor_read_key(state);
    switch (c) {
    case CTRL_KEY('q'):
        if (state->dirty) {
            editor_set_status(state, "WARNING: File has unsaved changes. "
                                     "Press y to confirm exit / s to save and exit.");
            editor_refresh_screen(state);
            int j = editor_read_key(state);
            if (j == 'y') {
                    clean_exit("Exit called by user, changes not saved\r\n", state);
            }
            else if (j == 's') {
                editor_save_file(state);
                clean_exit("", state);
            }
            else {
                editor_set_status(state, "Cancelled.");
                break;
            }
        } else {
            clean_exit("Exit called by user.\r\n", state);
        }
    break;
    case CTRL_KEY('s'):
        editor_save_file(state);
    break;
    case 33:
        editor_save_file(state);
        clean_exit("Exit called by user.\r\n", state);
    break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(state, c);
    break;
    case PAGE_UP:
    case PAGE_DOWN: {
            if (c == PAGE_UP) {
                state->cy = state->row_offset;
            }
            else if (c == PAGE_DOWN) {
                state->cy = state->row_offset + state->rows - 1;
                if (state->cy > state->n_rows)
                    state->cy = state->n_rows;
            }
            int times = state->rows;
            while (times--)
                editor_move_cursor(state, c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    break;
    case HOME:
        state->cx = 0;
        break;
    case END:
        if (state->cy < state->n_rows)
            state->cx = state->row[state->cy].size;
    break;
    case '\r':
        if (state->cy == state->n_rows) {
            editor_insert_row(state, "", 0, state->n_rows);
            state->cy++;
            state->cx = 0;
        }
        else {
            editor_insert_newline(state);
        }
    break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL:
        if (c == DEL)
            editor_move_cursor(state, ARROW_RIGHT);
        editor_delete_char(state);
        break;
    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default: editor_insert_char(state, c);
        break;
    }
    state->last_key = c;
}

void editor_move_cursor(struct editor_state *state, int key) {
    e_row *row = (state->cy >= state->n_rows) ? NULL : &state->row[state->cy];
    switch (key) {
    case ARROW_LEFT:
        if (state->cx != 0) {
            state->cx--;
        }
        else if (state->cx == 0) {
            if (state->cy > 0) {
                state->cy--;
                state->cx = state->row[state->cy].size;
            }
        }
        break;
    case ARROW_RIGHT:
        if (row && state->cx < row->size && state->cy < state->n_rows) {
            state->cx++;
        }
        else if (state->cy == state->n_rows) {
        }
        break;
    case ARROW_UP:
        if (state->cy != 0) state->cy--;
        break;
    case ARROW_DOWN:
        if (state->cy < state->n_rows - 1) state->cy++;
        if (!row) {
            return;
        }
        break;
    }
    row = (state->cy >= state->n_rows) ? NULL : &state->row[state->cy];
    if (!row) return;
    size_t row_len = row ? row->size : 0;
    if (state->cx > row_len) {
        state->cx = row_len;
    }
}

char *editor_rows_to_string(struct editor_state *state, int *buf_len) {
    int tot_len = 0;
    for (size_t j = 0; j < state->n_rows; j++)
        tot_len += state->row[j].size + 1;
    *buf_len = tot_len;
    char *buf = malloc(tot_len);
    char *p = buf;
    for (size_t j = 0; j < state->n_rows; j++) {
        memcpy(p, state->row[j].chars, state->row[j].size);
        p += state->row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editor_update_row(e_row *row) {
    int tabs = 0;
    for (size_t j = 0; j <= row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + (tabs * (TAB_SIZE - 1)) + 1);

    int idx = 0;
    for (size_t j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_SIZE != 0) row->render[idx++] = ' ';
        }
        else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->render_size = idx;
}

int e_row_cx_to_rx(e_row *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (TAB_SIZE - 1) - (rx % TAB_SIZE);
        rx++;
    }
    return rx;
}

void e_row_insert_char(struct editor_state *state, e_row *row, size_t at, int c) {
    if (at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    state->dirty++;
}

void editor_insert_char(struct editor_state *state, int c) {
    if (state->cy == state->n_rows || state->cy == state->n_rows - 1) {
        editor_insert_row(state, "", 0, state->n_rows);
    }
    e_row_insert_char(state, &state->row[state->cy], state->cx, c);
    state->cx++;
}

void e_row_delete_char(struct editor_state *state, e_row *row, size_t at) {
    if (at >= row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    state->dirty++;
}

void editor_delete_char(struct editor_state *state) {
    if (state->cy == state->n_rows) {
        return;
    }
    if (state->cx == 0 && state->cy == 0)
        return;
    e_row *row = &state->row[state->cy];
    if (state->cx > 0) {
        state->cx--;
        e_row_delete_char(state, row, state->cx);
    } else {
        state->cx = state->row[state->cy - 1].size;
        editor_row_append_string(state, &state->row[state->cy - 1], row->chars, row->size);
        editor_delete_row(state, state->cy);
        state->cy -= 1;
    }
}

void editor_free_row(e_row *row) {
    free(row->render);
    free(row->chars);
}
void editor_delete_row(struct editor_state *state, size_t at) {
    if (at >= state->n_rows)
        return;
    editor_free_row(&state->row[at]);
    memmove(&state->row[at], &state->row[at + 1], sizeof(e_row) * (state->n_rows - at - 1));
    state->n_rows--;
    state->dirty++;
}

void editor_row_append_string(struct editor_state *state, e_row *row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    state->dirty++;
}

void editor_insert_newline(struct editor_state *state) {
  if (state->cx == 0) {
      editor_insert_row(state, "", 0, state->cy);
  } else {
    e_row *row = &state->row[state->cy];
    editor_insert_row(state, &row->chars[state->cx], row->size - state->cx, state->cy + 1);
    row = &state->row[state->cy];
    row->size = state->cx;
    row->chars[row->size] = '\0';
    editor_update_row(row);
  }
  state->cy++;
  state->cx = 0;
}

char *e_get_prompt_response(struct editor_state *state) {
    size_t buf_size = 128;
    size_t buf_len = 0;
    char *buf = malloc(128 + 1);
    if (buf) {
        int c;
        while (1) {
            editor_set_status(state, "%s", buf);
            editor_refresh_screen(state);
            c = editor_read_key(state);

            if (c == DEL || c == CTRL_KEY('h') || c == BACKSPACE) {
                if (buf_len != 0) buf[--buf_len] = '\0';
            } 
            else if (c == '\x1b') {
                editor_set_status(state, "");
                free(buf);
                return NULL;
            }
            else if (c == '\r') {
                    if (buf_len != 0) {
                        editor_set_status(state, "");
                        return buf;
                    }
                }
                else if (!iscntrl(c) && c < 128) {
                    if (buf_len == buf_size - 1) {
                        buf_size *= 2;
                        buf = realloc(buf, buf_size);
                    }
                    buf[buf_len++] = c;
                    buf[buf_len] = '\0';
                }
            }
        buf[buf_len] = '\0';
        editor_set_status(state, "input: %s", buf);
        editor_refresh_screen(state);
        return buf;
    }
    else {
        return NULL;
    }
}
