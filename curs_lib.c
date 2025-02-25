/**
 * @file
 * GUI miscellaneous curses (window drawing) routines
 *
 * @authors
 * Copyright (C) 1996-2002,2010,2012-2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page curs_lib GUI miscellaneous curses (window drawing) routines
 *
 * GUI miscellaneous curses (window drawing) routines
 */

#include "config.h"
#include <stddef.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include "mutt/mutt.h"
#include "config/lib.h"
#include "email/lib.h"
#include "mutt.h"
#include "curs_lib.h"
#include "browser.h"
#include "context.h"
#include "enter_state.h"
#include "globals.h"
#include "mailbox.h"
#include "mutt_curses.h"
#include "mutt_logging.h"
#include "mutt_menu.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "opcodes.h"
#include "options.h"
#include "pager.h"
#include "protos.h"
#include "sidebar.h"
#ifdef HAVE_ISWBLANK
#include <wctype.h>
#endif
#ifdef USE_INOTIFY
#include "monitor.h"
#endif

/* These Config Variables are only used in curs_lib.c */
bool C_MetaKey; ///< Config: Interpret 'ALT-x' as 'ESC-x'

/* not possible to unget more than one char under some curses libs, and it
 * is impossible to unget function keys in SLang, so roll our own input
 * buffering routines.
 */

/* These are used for macros and exec/push commands.
 * They can be temporarily ignored by setting OptIgnoreMacroEvents
 */
static size_t MacroBufferCount = 0;
static size_t MacroBufferLen = 0;
static struct KeyEvent *MacroEvents;

/* These are used in all other "normal" situations, and are not
 * ignored when setting OptIgnoreMacroEvents
 */
static size_t UngetCount = 0;
static size_t UngetLen = 0;
static struct KeyEvent *UngetKeyEvents;

int MuttGetchTimeout = -1;

/**
 * mutt_refresh - Force a refresh of the screen
 */
void mutt_refresh(void)
{
  /* don't refresh when we are waiting for a child. */
  if (OptKeepQuiet)
    return;

  /* don't refresh in the middle of macros unless necessary */
  if (MacroBufferCount && !OptForceRefresh && !OptIgnoreMacroEvents)
    return;

  /* else */
  refresh();
}

/**
 * mutt_need_hard_redraw - Force a hard refresh
 *
 * Make sure that the next refresh does a full refresh.  This could be
 * optimized by not doing it at all if DISPLAY is set as this might indicate
 * that a GUI based pinentry was used.  Having an option to customize this is
 * of course the NeoMutt way.
 */
void mutt_need_hard_redraw(void)
{
  keypad(stdscr, true);
  clearok(stdscr, true);
  mutt_menu_set_current_redraw_full();
}

/**
 * mutt_getch_timeout - Set the getch() timeout
 * @param delay Timeout delay in ms
 *
 * delay is just like for timeout() or poll(): the number of milliseconds
 * mutt_getch() should block for input.
 * * delay == 0 means mutt_getch() is non-blocking.
 * * delay < 0 means mutt_getch is blocking.
 */
void mutt_getch_timeout(int delay)
{
  MuttGetchTimeout = delay;
  timeout(delay);
}

#ifdef USE_INOTIFY
/**
 * mutt_monitor_getch - Get a character and poll the filesystem monitor
 * @retval num Character pressed
 * @retval ERR Timeout
 */
static int mutt_monitor_getch(void)
{
  /* ncurses has its own internal buffer, so before we perform a poll,
   * we need to make sure there isn't a character waiting */
  timeout(0);
  int ch = getch();
  timeout(MuttGetchTimeout);
  if (ch == ERR)
  {
    if (mutt_monitor_poll() != 0)
      ch = ERR;
    else
      ch = getch();
  }
  return ch;
}
#endif /* USE_INOTIFY */

/**
 * mutt_getch - Read a character from the input buffer
 * @retval obj KeyEvent to process
 *
 * The priority for reading events is:
 * 1. UngetKeyEvents buffer
 * 2. MacroEvents buffer
 * 3. Keyboard
 *
 * This function can return:
 * - Error `{ -1, OP_NULL }`
 * - Timeout `{ -2, OP_NULL }`
 */
struct KeyEvent mutt_getch(void)
{
  int ch;
  struct KeyEvent err = { -1, OP_NULL }, ret;
  struct KeyEvent timeout = { -2, OP_NULL };

  if (UngetCount)
    return UngetKeyEvents[--UngetCount];

  if (!OptIgnoreMacroEvents && MacroBufferCount)
    return MacroEvents[--MacroBufferCount];

  SigInt = 0;

  mutt_sig_allow_interrupt(1);
#ifdef KEY_RESIZE
  /* ncurses 4.2 sends this when the screen is resized */
  ch = KEY_RESIZE;
  while (ch == KEY_RESIZE)
#endif /* KEY_RESIZE */
#ifdef USE_INOTIFY
    ch = mutt_monitor_getch();
#else
  ch = getch();
#endif /* USE_INOTIFY */
  mutt_sig_allow_interrupt(0);

  if (SigInt)
  {
    mutt_query_exit();
    return err;
  }

  /* either timeout, a sigwinch (if timeout is set), or the terminal
   * has been lost */
  if (ch == ERR)
  {
    if (!isatty(0))
      mutt_exit(1);

    return timeout;
  }

  if ((ch & 0x80) && C_MetaKey)
  {
    /* send ALT-x as ESC-x */
    ch &= ~0x80;
    mutt_unget_event(ch, 0);
    ret.ch = '\033'; // Escape
    ret.op = 0;
    return ret;
  }

  ret.ch = ch;
  ret.op = 0;
  return (ch == ctrl('G')) ? err : ret;
}

/**
 * mutt_get_field_full - Ask the user for a string
 * @param[in]  field    Prompt
 * @param[in]  buf      Buffer for the result
 * @param[in]  buflen   Length of buffer
 * @param[in]  complete Flags, see #CompletionFlags
 * @param[in]  multiple Allow multiple selections
 * @param[out] files    List of files selected
 * @param[out] numfiles Number of files selected
 * @retval 1  Redraw the screen and call the function again
 * @retval 0  Selection made
 * @retval -1 Aborted
 */
int mutt_get_field_full(const char *field, char *buf, size_t buflen, CompletionFlags complete,
                        bool multiple, char ***files, int *numfiles)
{
  int ret;
  int x;

  struct EnterState *es = mutt_enter_state_new();

  do
  {
    if (SigWinch)
    {
      SigWinch = 0;
      mutt_resize_screen();
      clearok(stdscr, TRUE);
      mutt_menu_current_redraw();
    }
    mutt_window_clearline(MuttMessageWindow, 0);
    SET_COLOR(MT_COLOR_PROMPT);
    addstr(field);
    NORMAL_COLOR;
    mutt_refresh();
    mutt_window_getxy(MuttMessageWindow, &x, NULL);
    ret = mutt_enter_string_full(buf, buflen, x, complete, multiple, files, numfiles, es);
  } while (ret == 1);
  mutt_window_clearline(MuttMessageWindow, 0);
  mutt_enter_state_free(&es);

  return ret;
}

/**
 * mutt_get_field_unbuffered - Ask the user for a string (ignoring macro buffer)
 * @param msg    Prompt
 * @param buf    Buffer for the result
 * @param buflen Length of buffer
 * @param flags  Flags, see #CompletionFlags
 * @retval 1  Redraw the screen and call the function again
 * @retval 0  Selection made
 * @retval -1 Aborted
 */
int mutt_get_field_unbuffered(const char *msg, char *buf, size_t buflen, CompletionFlags flags)
{
  int rc;

  OptIgnoreMacroEvents = true;
  rc = mutt_get_field(msg, buf, buflen, flags);
  OptIgnoreMacroEvents = false;

  return rc;
}

/**
 * mutt_edit_file - Let the user edit a file
 * @param editor User's editor config
 * @param file   File to edit
 */
void mutt_edit_file(const char *editor, const char *file)
{
  struct Buffer *cmd = mutt_buffer_pool_get();

  mutt_endwin();
  mutt_buffer_file_expand_fmt_quote(cmd, editor, file);
  if (mutt_system(mutt_b2s(cmd)) != 0)
  {
    mutt_error(_("Error running \"%s\""), mutt_b2s(cmd));
  }
  /* the terminal may have been resized while the editor owned it */
  mutt_resize_screen();
  keypad(stdscr, true);
  clearok(stdscr, true);

  mutt_buffer_pool_release(&cmd);
}

/**
 * mutt_yesorno - Ask the user a Yes/No question
 * @param msg Prompt
 * @param def Default answer, see #QuadOption
 * @retval num Selection made, see #QuadOption
 */
enum QuadOption mutt_yesorno(const char *msg, enum QuadOption def)
{
  struct KeyEvent ch;
  char *yes = _("yes");
  char *no = _("no");
  char *answer_string = NULL;
  int answer_string_wid, msg_wid;
  size_t trunc_msg_len;
  bool redraw = true;
  int prompt_lines = 1;

  char *expr = NULL;
  regex_t reyes;
  regex_t reno;
  char answer[2];

  answer[1] = '\0';

  bool reyes_ok = (expr = nl_langinfo(YESEXPR)) && (expr[0] == '^') &&
                  (REG_COMP(&reyes, expr, REG_NOSUB) == 0);
  bool reno_ok = (expr = nl_langinfo(NOEXPR)) && (expr[0] == '^') &&
                 (REG_COMP(&reno, expr, REG_NOSUB) == 0);

  /* In order to prevent the default answer to the question to wrapped
   * around the screen in the even the question is wider than the screen,
   * ensure there is enough room for the answer and truncate the question
   * to fit.  */
  mutt_str_asprintf(&answer_string, " ([%s]/%s): ", (def == MUTT_YES) ? yes : no,
                    (def == MUTT_YES) ? no : yes);
  answer_string_wid = mutt_strwidth(answer_string);
  msg_wid = mutt_strwidth(msg);

  while (true)
  {
    if (redraw || SigWinch)
    {
      redraw = false;
      if (SigWinch)
      {
        SigWinch = 0;
        mutt_resize_screen();
        clearok(stdscr, TRUE);
        mutt_menu_current_redraw();
      }
      if (MuttMessageWindow->cols)
      {
        prompt_lines = (msg_wid + answer_string_wid + MuttMessageWindow->cols - 1) /
                       MuttMessageWindow->cols;
        prompt_lines = MAX(1, MIN(3, prompt_lines));
      }
      if (prompt_lines != MuttMessageWindow->rows)
      {
        mutt_window_reflow_message_rows(prompt_lines);
        mutt_menu_current_redraw();
      }

      /* maxlen here is sort of arbitrary, so pick a reasonable upper bound */
      trunc_msg_len = mutt_wstr_trunc(
          msg, 4 * prompt_lines * MuttMessageWindow->cols,
          prompt_lines * MuttMessageWindow->cols - answer_string_wid, NULL);

      mutt_window_move(MuttMessageWindow, 0, 0);
      SET_COLOR(MT_COLOR_PROMPT);
      addnstr(msg, trunc_msg_len);
      addstr(answer_string);
      NORMAL_COLOR;
      mutt_window_clrtoeol(MuttMessageWindow);
    }

    mutt_refresh();
    /* SigWinch is not processed unless timeout is set */
    mutt_getch_timeout(30 * 1000);
    ch = mutt_getch();
    mutt_getch_timeout(-1);
    if (ch.ch == -2)
      continue;
    if (CI_is_return(ch.ch))
      break;
    if (ch.ch < 0)
    {
      def = MUTT_ABORT;
      break;
    }

    answer[0] = ch.ch;
    if (reyes_ok ? (regexec(&reyes, answer, 0, 0, 0) == 0) : (tolower(ch.ch) == 'y'))
    {
      def = MUTT_YES;
      break;
    }
    else if (reno_ok ? (regexec(&reno, answer, 0, 0, 0) == 0) : (tolower(ch.ch) == 'n'))
    {
      def = MUTT_NO;
      break;
    }
    else
    {
      BEEP();
    }
  }

  FREE(&answer_string);

  if (reyes_ok)
    regfree(&reyes);
  if (reno_ok)
    regfree(&reno);

  if (MuttMessageWindow->rows != 1)
  {
    mutt_window_reflow_message_rows(1);
    mutt_menu_current_redraw();
  }
  else
    mutt_window_clearline(MuttMessageWindow, 0);

  if (def != MUTT_ABORT)
  {
    addstr((char *) ((def == MUTT_YES) ? yes : no));
    mutt_refresh();
  }
  else
  {
    /* when the users cancels with ^G, clear the message stored with
     * mutt_message() so it isn't displayed when the screen is refreshed. */
    mutt_clear_error();
  }
  return def;
}

/**
 * mutt_query_exit - Ask the user if they want to leave NeoMutt
 *
 * This function is called when the user presses the abort key.
 */
void mutt_query_exit(void)
{
  mutt_flushinp();
  curs_set(1);
  if (C_Timeout)
    mutt_getch_timeout(-1); /* restore blocking operation */
  if (mutt_yesorno(_("Exit NeoMutt?"), MUTT_YES) == MUTT_YES)
  {
    mutt_exit(1);
  }
  mutt_clear_error();
  mutt_curs_set(-1);
  SigInt = 0;
}

/**
 * mutt_show_error - Show the user an error message
 */
void mutt_show_error(void)
{
  if (OptKeepQuiet || !ErrorBufMessage)
    return;

  SET_COLOR(OptMsgErr ? MT_COLOR_ERROR : MT_COLOR_MESSAGE);
  mutt_window_mvaddstr(MuttMessageWindow, 0, 0, ErrorBuf);
  NORMAL_COLOR;
  mutt_window_clrtoeol(MuttMessageWindow);
}

/**
 * mutt_endwin - Shutdown curses/slang
 */
void mutt_endwin(void)
{
  if (OptNoCurses)
    return;

  int e = errno;

  /* at least in some situations (screen + xterm under SuSE11/12) endwin()
   * doesn't properly flush the screen without an explicit call.  */
  mutt_refresh();
  endwin();

  errno = e;
}

/**
 * mutt_perror_debug - Show the user an 'errno' message
 * @param s Additional text to show
 */
void mutt_perror_debug(const char *s)
{
  char *p = strerror(errno);

  mutt_debug(LL_DEBUG1, "%s: %s (errno = %d)\n", s, p ? p : "unknown error", errno);
  mutt_error("%s: %s (errno = %d)", s, p ? p : _("unknown error"), errno);
}

/**
 * mutt_flush_stdin - remove characters from stdin until '\n' or EOF is encountered
 */
void mutt_flush_stdin(void)
{
  int c;
  do 
  {
    c = fgetc(stdin);
  } while ((c != '\n') && (c != EOF));
}


/**
 * mutt_any_key_to_continue - Prompt the user to 'press any key' and wait
 * @param s Message prompt
 * @retval num Key pressed
 * @retval EOF Error, or prompt aborted
 */
int mutt_any_key_to_continue(const char *s)
{
  struct termios t;
  struct termios old;

  int fd = open("/dev/tty", O_RDONLY);
  if (fd < 0)
    return EOF;
  tcgetattr(fd, &t);
  memcpy((void *) &old, (void *) &t, sizeof(struct termios)); /* save original state */
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSADRAIN, &t);
  fflush(stdout);
  if (s)
    fputs(s, stdout);
  else
    fputs(_("Press any key to continue..."), stdout);
  fflush(stdout);
  int ch = fgetc(stdin);
  mutt_flush_stdin();
  tcsetattr(fd, TCSADRAIN, &old);
  close(fd);
  fputs("\r\n", stdout);
  mutt_clear_error();
  return (ch >= 0) ? ch : EOF;
}

/**
 * mutt_do_pager - Display some page-able text to the user
 * @param banner   Message for status bar
 * @param tempfile File to display
 * @param do_color Flags, see #PagerFlags
 * @param info     Info about current mailbox (OPTIONAL)
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_do_pager(const char *banner, const char *tempfile, PagerFlags do_color,
                  struct Pager *info)
{
  int rc;

  if (!C_Pager || (mutt_str_strcmp(C_Pager, "builtin") == 0))
    rc = mutt_pager(banner, tempfile, do_color, info);
  else
  {
    struct Buffer *cmd = mutt_buffer_pool_get();

    mutt_endwin();
    mutt_buffer_file_expand_fmt_quote(cmd, C_Pager, tempfile);
    if (mutt_system(mutt_b2s(cmd)) == -1)
    {
      mutt_error(_("Error running \"%s\""), mutt_b2s(cmd));
      rc = -1;
    }
    else
      rc = 0;
    mutt_file_unlink(tempfile);
    mutt_buffer_pool_release(&cmd);
  }

  return rc;
}

/**
 * mutt_enter_fname_full - Ask the user to select a file
 * @param[in]  prompt   Prompt
 * @param[in]  buf      Buffer for the result
 * @param[in]  buflen   Length of the buffer
 * @param[in]  mailbox  If true, select mailboxes
 * @param[in]  multiple Allow multiple selections
 * @param[out] files    List of files selected
 * @param[out] numfiles Number of files selected
 * @param[in]  flags    Flags, see #SelectFileFlags
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_enter_fname_full(const char *prompt, char *buf, size_t buflen, bool mailbox,
                          bool multiple, char ***files, int *numfiles, SelectFileFlags flags)
{
  struct Buffer *fname = mutt_buffer_pool_get();

  mutt_buffer_addstr(fname, NONULL(buf));
  int rc = mutt_buffer_enter_fname_full(prompt, fname, mailbox, multiple, files,
                                        numfiles, flags);
  mutt_str_strfcpy(buf, mutt_b2s(fname), buflen);

  mutt_buffer_pool_release(&fname);
  return rc;
}

/**
 * mutt_buffer_enter_fname_full - Ask the user to select a file
 * @param[in]  prompt   Prompt
 * @param[in]  fname    Buffer for the result
 * @param[in]  mailbox  If true, select mailboxes
 * @param[in]  multiple Allow multiple selections
 * @param[out] files    List of files selected
 * @param[out] numfiles Number of files selected
 * @param[in]  flags    Flags, see #SelectFileFlags
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_buffer_enter_fname_full(const char *prompt, struct Buffer *fname,
                                 bool mailbox, bool multiple, char ***files,
                                 int *numfiles, SelectFileFlags flags)
{
  struct KeyEvent ch;

  SET_COLOR(MT_COLOR_PROMPT);
  mutt_window_mvaddstr(MuttMessageWindow, 0, 0, (char *) prompt);
  addstr(_(" ('?' for list): "));
  NORMAL_COLOR;
  if (mutt_buffer_len(fname))
    addstr(mutt_b2s(fname));
  mutt_window_clrtoeol(MuttMessageWindow);
  mutt_refresh();

  do
  {
    ch = mutt_getch();
  } while (ch.ch == -2);
  if (ch.ch < 0)
  {
    mutt_window_clearline(MuttMessageWindow, 0);
    return -1;
  }
  else if (ch.ch == '?')
  {
    mutt_refresh();
    mutt_buffer_reset(fname);

    if (flags == MUTT_SEL_NO_FLAGS)
      flags = MUTT_SEL_FOLDER;
    if (multiple)
      flags |= MUTT_SEL_MULTI;
    if (mailbox)
      flags |= MUTT_SEL_MAILBOX;
    mutt_buffer_select_file(fname, flags, files, numfiles);
  }
  else
  {
    char *pc = mutt_mem_malloc(mutt_str_strlen(prompt) + 3);

    sprintf(pc, "%s: ", prompt);
    if (ch.op == OP_NULL)
      mutt_unget_event(ch.ch, 0);
    else
      mutt_unget_event(0, ch.op);

    mutt_buffer_increase_size(fname, 1024);
    if (mutt_get_field_full(pc, fname->data, fname->dsize,
                            (mailbox ? MUTT_EFILE : MUTT_FILE) | MUTT_CLEAR,
                            multiple, files, numfiles) != 0)
      mutt_buffer_reset(fname);
    else
      mutt_buffer_fix_dptr(fname);
    FREE(&pc);
  }

  return 0;
}

/**
 * mutt_unget_event - Return a keystroke to the input buffer
 * @param ch Key press
 * @param op Operation, e.g. OP_DELETE
 *
 * This puts events into the `UngetKeyEvents` buffer
 */
void mutt_unget_event(int ch, int op)
{
  struct KeyEvent tmp;

  tmp.ch = ch;
  tmp.op = op;

  if (UngetCount >= UngetLen)
    mutt_mem_realloc(&UngetKeyEvents, (UngetLen += 16) * sizeof(struct KeyEvent));

  UngetKeyEvents[UngetCount++] = tmp;
}

/**
 * mutt_unget_string - Return a string to the input buffer
 * @param s String to return
 *
 * This puts events into the `UngetKeyEvents` buffer
 */
void mutt_unget_string(const char *s)
{
  const char *p = s + mutt_str_strlen(s) - 1;

  while (p >= s)
  {
    mutt_unget_event((unsigned char) *p--, 0);
  }
}

/**
 * mutt_push_macro_event - Add the character/operation to the macro buffer
 * @param ch Character to add
 * @param op Operation to add
 *
 * Adds the ch/op to the macro buffer.
 * This should be used for macros, push, and exec commands only.
 */
void mutt_push_macro_event(int ch, int op)
{
  struct KeyEvent tmp;

  tmp.ch = ch;
  tmp.op = op;

  if (MacroBufferCount >= MacroBufferLen)
    mutt_mem_realloc(&MacroEvents, (MacroBufferLen += 128) * sizeof(struct KeyEvent));

  MacroEvents[MacroBufferCount++] = tmp;
}

/**
 * mutt_flush_macro_to_endcond - Drop a macro from the input buffer
 *
 * All the macro text is deleted until an OP_END_COND command,
 * or the buffer is empty.
 */
void mutt_flush_macro_to_endcond(void)
{
  UngetCount = 0;
  while (MacroBufferCount > 0)
  {
    if (MacroEvents[--MacroBufferCount].op == OP_END_COND)
      return;
  }
}

/**
 * mutt_flush_unget_to_endcond - Clear entries from UngetKeyEvents
 *
 * Normally, OP_END_COND should only be in the MacroEvent buffer.
 * km_error_key() (ab)uses OP_END_COND as a barrier in the unget buffer, and
 * calls this function to flush.
 */
void mutt_flush_unget_to_endcond(void)
{
  while (UngetCount > 0)
  {
    if (UngetKeyEvents[--UngetCount].op == OP_END_COND)
      return;
  }
}

/**
 * mutt_flushinp - Empty all the keyboard buffers
 */
void mutt_flushinp(void)
{
  UngetCount = 0;
  MacroBufferCount = 0;
  flushinp();
}

#if (defined(USE_SLANG_CURSES) || defined(HAVE_CURS_SET))
/**
 * mutt_curs_set - Set the cursor position
 * @param cursor
 * * -1: restore the value of the last call
 * *  0: make the cursor invisible
 * *  1: make the cursor visible
 */
void mutt_curs_set(int cursor)
{
  static int SavedCursor = 1;

  if (cursor < 0)
    cursor = SavedCursor;
  else
    SavedCursor = cursor;

  if (curs_set(cursor) == ERR)
  {
    if (cursor == 1) /* cnorm */
      curs_set(2);   /* cvvis */
  }
}
#endif

/**
 * mutt_multi_choice - Offer the user a multiple choice question
 * @param prompt  Message prompt
 * @param letters Allowable selection keys
 * @retval >=0 0-based user selection
 * @retval  -1 Selection aborted
 */
int mutt_multi_choice(const char *prompt, const char *letters)
{
  struct KeyEvent ch;
  int choice;
  bool redraw = true;
  int prompt_lines = 1;

  while (true)
  {
    if (redraw || SigWinch)
    {
      redraw = false;
      if (SigWinch)
      {
        SigWinch = 0;
        mutt_resize_screen();
        clearok(stdscr, TRUE);
        mutt_menu_current_redraw();
      }
      if (MuttMessageWindow->cols)
      {
        prompt_lines = (mutt_strwidth(prompt) + MuttMessageWindow->cols - 1) /
                       MuttMessageWindow->cols;
        prompt_lines = MAX(1, MIN(3, prompt_lines));
      }
      if (prompt_lines != MuttMessageWindow->rows)
      {
        mutt_window_reflow_message_rows(prompt_lines);
        mutt_menu_current_redraw();
      }

      mutt_window_move(MuttMessageWindow, 0, 0);

      if ((ColorDefs[MT_COLOR_OPTIONS] != 0) &&
          (ColorDefs[MT_COLOR_OPTIONS] != ColorDefs[MT_COLOR_PROMPT]))
      {
        char *cur = NULL;

        while ((cur = strchr(prompt, '(')))
        {
          // write the part between prompt and cur using MT_COLOR_PROMPT
          SET_COLOR(MT_COLOR_PROMPT);
          addnstr(prompt, cur - prompt);

          if (isalnum(cur[1]) && (cur[2] == ')'))
          {
            // we have a single letter within parentheses
            SET_COLOR(MT_COLOR_OPTIONS);
            addch(cur[1]);
            prompt = cur + 3;
          }
          else
          {
            // we have a parenthesis followed by something else
            addch(cur[0]);
            prompt = cur + 1;
          }
        }
      }

      SET_COLOR(MT_COLOR_PROMPT);
      addstr(prompt);
      NORMAL_COLOR;

      addch(' ');
      mutt_window_clrtoeol(MuttMessageWindow);
    }

    mutt_refresh();
    /* SigWinch is not processed unless timeout is set */
    mutt_getch_timeout(30 * 1000);
    ch = mutt_getch();
    mutt_getch_timeout(-1);
    if (ch.ch == -2)
      continue;
    /* (ch.ch == 0) is technically possible.  Treat the same as < 0 (abort) */
    if ((ch.ch <= 0) || CI_is_return(ch.ch))
    {
      choice = -1;
      break;
    }
    else
    {
      char *p = strchr(letters, ch.ch);
      if (p)
      {
        choice = p - letters + 1;
        break;
      }
      else if ((ch.ch <= '9') && (ch.ch > '0'))
      {
        choice = ch.ch - '0';
        if (choice <= mutt_str_strlen(letters))
          break;
      }
    }
    BEEP();
  }
  if (MuttMessageWindow->rows != 1)
  {
    mutt_window_reflow_message_rows(1);
    mutt_menu_current_redraw();
  }
  else
    mutt_window_clearline(MuttMessageWindow, 0);
  mutt_refresh();
  return choice;
}

/**
 * mutt_addwch - addwch would be provided by an up-to-date curses library
 * @param wc Wide char to display
 * @retval  0 Success
 * @retval -1 Error
 */
int mutt_addwch(wchar_t wc)
{
  char buf[MB_LEN_MAX * 2];
  mbstate_t mbstate;
  size_t n1, n2;

  memset(&mbstate, 0, sizeof(mbstate));
  if (((n1 = wcrtomb(buf, wc, &mbstate)) == (size_t)(-1)) ||
      ((n2 = wcrtomb(buf + n1, 0, &mbstate)) == (size_t)(-1)))
  {
    return -1; /* ERR */
  }
  else
  {
    return addstr(buf);
  }
}

/**
 * mutt_simple_format - Format a string, like snprintf()
 * @param[out] buf       Buffer in which to save string
 * @param[in]  buflen    Buffer length
 * @param[in]  min_width Minimum width
 * @param[in]  max_width Maximum width
 * @param[in]  justify   Justification, e.g. #JUSTIFY_RIGHT
 * @param[in]  pad_char  Padding character
 * @param[in]  s         String to format
 * @param[in]  n         Number of bytes of string to format
 * @param[in]  arboreal  If true, string contains graphical tree characters
 *
 * This formats a string, a bit like snprintf(buf, buflen, "%-*.*s",
 * min_width, max_width, s), except that the widths refer to the number of
 * character cells when printed.
 */
void mutt_simple_format(char *buf, size_t buflen, int min_width, int max_width,
                        enum FormatJustify justify, char pad_char,
                        const char *s, size_t n, bool arboreal)
{
  wchar_t wc;
  int w;
  size_t k, k2;
  char scratch[MB_LEN_MAX];
  mbstate_t mbstate1, mbstate2;
  int escaped = 0;

  memset(&mbstate1, 0, sizeof(mbstate1));
  memset(&mbstate2, 0, sizeof(mbstate2));
  buflen--;
  char *p = buf;
  for (; n && (k = mbrtowc(&wc, s, n, &mbstate1)); s += k, n -= k)
  {
    if ((k == (size_t)(-1)) || (k == (size_t)(-2)))
    {
      if ((k == (size_t)(-1)) && (errno == EILSEQ))
        memset(&mbstate1, 0, sizeof(mbstate1));

      k = (k == (size_t)(-1)) ? 1 : n;
      wc = ReplacementChar;
    }
    if (escaped)
    {
      escaped = 0;
      w = 0;
    }
    else if (arboreal && (wc == MUTT_SPECIAL_INDEX))
    {
      escaped = 1;
      w = 0;
    }
    else if (arboreal && (wc < MUTT_TREE_MAX))
    {
      w = 1; /* hack */
    }
    else
    {
#ifdef HAVE_ISWBLANK
      if (iswblank(wc))
        wc = ' ';
      else
#endif
          if (!IsWPrint(wc))
        wc = '?';
      w = wcwidth(wc);
    }
    if (w >= 0)
    {
      if ((w > max_width) || ((k2 = wcrtomb(scratch, wc, &mbstate2)) > buflen))
        continue;
      min_width -= w;
      max_width -= w;
      strncpy(p, scratch, k2);
      p += k2;
      buflen -= k2;
    }
  }
  w = ((int) buflen < min_width) ? buflen : min_width;
  if (w <= 0)
    *p = '\0';
  else if (justify == JUSTIFY_RIGHT) /* right justify */
  {
    p[w] = '\0';
    while (--p >= buf)
      p[w] = *p;
    while (--w >= 0)
      buf[w] = pad_char;
  }
  else if (justify == JUSTIFY_CENTER) /* center */
  {
    char *savedp = p;
    int half = (w + 1) / 2; /* half of cushion space */

    p[w] = '\0';

    /* move str to center of buffer */
    while (--p >= buf)
      p[half] = *p;

    /* fill rhs */
    p = savedp + half;
    while (--w >= half)
      *p++ = pad_char;

    /* fill lhs */
    while (half--)
      buf[half] = pad_char;
  }
  else /* left justify */
  {
    while (--w >= 0)
      *p++ = pad_char;
    *p = '\0';
  }
}

/**
 * mutt_format_s_x - Format a string like snprintf()
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  prec     Field precision, e.g. "-3.4"
 * @param[in]  s        String to format
 * @param[in]  arboreal  If true, string contains graphical tree characters
 *
 * This formats a string rather like:
 * - snprintf(fmt, sizeof(fmt), "%%%ss", prec);
 * - snprintf(buf, buflen, fmt, s);
 * except that the numbers in the conversion specification refer to
 * the number of character cells when printed.
 */
void mutt_format_s_x(char *buf, size_t buflen, const char *prec, const char *s, bool arboreal)
{
  enum FormatJustify justify = JUSTIFY_RIGHT;
  char *p = NULL;
  int min_width;
  int max_width = INT_MAX;

  if (*prec == '-')
  {
    prec++;
    justify = JUSTIFY_LEFT;
  }
  else if (*prec == '=')
  {
    prec++;
    justify = JUSTIFY_CENTER;
  }
  min_width = strtol(prec, &p, 10);
  if (*p == '.')
  {
    prec = p + 1;
    max_width = strtol(prec, &p, 10);
    if (p <= prec)
      max_width = INT_MAX;
  }

  mutt_simple_format(buf, buflen, min_width, max_width, justify, ' ', s,
                     mutt_str_strlen(s), arboreal);
}

/**
 * mutt_format_s - Format a simple string
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  prec     Field precision, e.g. "-3.4"
 * @param[in]  s        String to format
 */
void mutt_format_s(char *buf, size_t buflen, const char *prec, const char *s)
{
  mutt_format_s_x(buf, buflen, prec, s, false);
}

/**
 * mutt_format_s_tree - Format a simple string with tree characters
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  prec     Field precision, e.g. "-3.4"
 * @param[in]  s        String to format
 */
void mutt_format_s_tree(char *buf, size_t buflen, const char *prec, const char *s)
{
  mutt_format_s_x(buf, buflen, prec, s, true);
}

/**
 * mutt_paddstr - Display a string on screen, padded if necessary
 * @param n Final width of field
 * @param s String to display
 */
void mutt_paddstr(int n, const char *s)
{
  wchar_t wc;
  size_t k;
  size_t len = mutt_str_strlen(s);
  mbstate_t mbstate;

  memset(&mbstate, 0, sizeof(mbstate));
  for (; len && (k = mbrtowc(&wc, s, len, &mbstate)); s += k, len -= k)
  {
    if ((k == (size_t)(-1)) || (k == (size_t)(-2)))
    {
      if (k == (size_t)(-1))
        memset(&mbstate, 0, sizeof(mbstate));
      k = (k == (size_t)(-1)) ? 1 : len;
      wc = ReplacementChar;
    }
    if (!IsWPrint(wc))
      wc = '?';
    const int w = wcwidth(wc);
    if (w >= 0)
    {
      if (w > n)
        break;
      addnstr((char *) s, k);
      n -= w;
    }
  }
  while (n-- > 0)
    addch(' ');
}

/**
 * mutt_wstr_trunc - Work out how to truncate a widechar string
 * @param[in]  src    String to measure
 * @param[in]  maxlen Maximum length of string in bytes
 * @param[in]  maxwid Maximum width in screen columns
 * @param[out] width  Save the truncated screen column width
 * @retval num Bytes to use
 *
 * See how many bytes to copy from string so it's at most maxlen bytes long and
 * maxwid columns wide
 */
size_t mutt_wstr_trunc(const char *src, size_t maxlen, size_t maxwid, size_t *width)
{
  wchar_t wc;
  size_t n, w = 0, l = 0, cl;
  int cw;
  mbstate_t mbstate;

  if (!src)
    goto out;

  n = mutt_str_strlen(src);

  memset(&mbstate, 0, sizeof(mbstate));
  for (w = 0; n && (cl = mbrtowc(&wc, src, n, &mbstate)); src += cl, n -= cl)
  {
    if ((cl == (size_t)(-1)) || (cl == (size_t)(-2)))
    {
      if (cl == (size_t)(-1))
        memset(&mbstate, 0, sizeof(mbstate));
      cl = (cl == (size_t)(-1)) ? 1 : n;
      wc = ReplacementChar;
    }
    cw = wcwidth(wc);
    /* hack because MUTT_TREE symbols aren't turned into characters
     * until rendered by print_enriched_string (#3364) */
    if ((cw < 0) && (src[0] == MUTT_SPECIAL_INDEX))
    {
      cl = 2; /* skip the index coloring sequence */
      cw = 0;
    }
    else if ((cw < 0) && (cl == 1) && (src[0] != '\0') && (src[0] < MUTT_TREE_MAX))
      cw = 1;
    else if (cw < 0)
      cw = 0; /* unprintable wchar */
    if ((cl + l > maxlen) || (cw + w > maxwid))
      break;
    l += cl;
    w += cw;
  }
out:
  if (width)
    *width = w;
  return l;
}

/**
 * mutt_strwidth - Measure a string's width in screen cells
 * @param s String to be measured
 * @retval num Screen cells string would use
 */
int mutt_strwidth(const char *s)
{
  if (!s)
    return 0;

  wchar_t wc;
  int w;
  size_t k;
  mbstate_t mbstate;

  size_t n = mutt_str_strlen(s);

  memset(&mbstate, 0, sizeof(mbstate));
  for (w = 0; n && (k = mbrtowc(&wc, s, n, &mbstate)); s += k, n -= k)
  {
    if (*s == MUTT_SPECIAL_INDEX)
    {
      s += 2; /* skip the index coloring sequence */
      k = 0;
      continue;
    }

    if ((k == (size_t)(-1)) || (k == (size_t)(-2)))
    {
      if (k == (size_t)(-1))
        memset(&mbstate, 0, sizeof(mbstate));
      k = (k == (size_t)(-1)) ? 1 : n;
      wc = ReplacementChar;
    }
    if (!IsWPrint(wc))
      wc = '?';
    w += wcwidth(wc);
  }
  return w;
}
