/*
  mairix - message index builder and finder for maildir folders.

 **********************************************************************
 * Copyright (C) Johannes Schindelin 2011
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **********************************************************************
 */

#include "mairix.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

char *git_dir = NULL;

struct hashtable {
  char **blob_names;
  int n, max;
};
#define char2hex(c) ((c) >= 'a' ? 10 + (c) - 'a' : (c) - '0')
static unsigned int hashfunc(char *blob_name)/*{{{*/
{
  /* we expect lower-case hex characters */
  return char2hex(blob_name[0]) |
    (char2hex(blob_name[1]) << 4) |
    (char2hex(blob_name[2]) << 8) |
    (char2hex(blob_name[3]) << 12) |
    (char2hex(blob_name[4]) << 16) |
    (char2hex(blob_name[5]) << 20) |
    (char2hex(blob_name[6]) << 24) |
    (char2hex(blob_name[7]) << 28);
}
/*}}}*/
static int add_to_hashtable(struct hashtable *table, char *blob_name)/*{{{*/
{
  int index;

  if (table->n + 1 > table->max / 2) {
    /* grow */
    int new_max = table->max > 128 ? table->max * 2 : 256, i;
    char **array = new_array(char *, new_max);
    struct hashtable new_table = { array, 0, new_max };

    memset(array, 0, sizeof(char *) * new_max);
    for (i = 0; i < table->max; i++)
      if (table->blob_names[i]) {
        add_to_hashtable(&new_table, table->blob_names[i]);
      }

    free(table->blob_names);
    table->blob_names = array;
    table->max = new_max;
  }

  index = hashfunc(blob_name) % table->max;
  while (table->blob_names[index]) {
    index++;
    if (index == table->max)
      index = 0;
  }

  table->blob_names[index] = blob_name;
  table->n++;
  return index;
}
/*}}}*/
static int hashtable_index(struct hashtable *table, char *blob_name)/*{{{*/
{
  int index;

  if (!table->n) {
    return -1;
  }

  index = hashfunc(blob_name) % table->max;
  while (table->blob_names[index]) {
    if (!strcmp(blob_name, table->blob_names[index])) {
      return index;
    }
    index++;
    if (index == table->max)
      index = 0;
  }

  return -1;
}
/*}}}*/
static void add_file_to_list(char *x, struct msgpath_array *arr) {/*{{{*/
  char *y = new_string(x);
  if (arr->n == arr->max) {
    arr->max += 1024;
    arr->paths = grow_array(struct msgpath, arr->max, arr->paths);
    arr->type  = grow_array(enum message_type, arr->max, arr->type);
  }
  arr->type[arr->n] = MTY_GITBLOB;
  arr->paths[arr->n].src.git.blob_name = y;
  ++arr->n;
  return;
}
/*}}}*/
void build_git_blob_lists(struct database *db,
   struct msgpath_array *msgs, const char *database_path)/*{{{*/
{
  struct hashtable table = { NULL, 0, 0 };
  int i, subprocess;
  struct stat st;
  char *argv[9] = {
    "git", "--git-dir", git_dir, "rev-list",
    "--objects", "HEAD", NULL, NULL, NULL
  };
  int fds[2];
  FILE *f;

  if (!git_dir) {
    return;
  }

  if (database_path && !stat(database_path, &st)) {
    int len = sizeof(argv) / sizeof(*argv);
    argv[len - 4] = "--since";
    argv[len - 3] = ctime(&st.st_mtime);
    argv[len - 2] = "HEAD";
  }

  for (i = 0; i < db->n_msgs; i++) {
    if (db->type[i] == MTY_GITBLOB &&
        hashtable_index(&table, db->msgs[i].src.git.blob_name) < 0) {
      add_to_hashtable(&table, db->msgs[i].src.git.blob_name);
    }
  }

  if (pipe(fds)) {
    perror("Could not set up a pipe");
    return;
  }

  subprocess = fork();
  if (subprocess < 0) {
    fprintf(stderr, "While trying to read objects: ");
    perror("Could not fork");
    exit(1);
  }
  if (!subprocess) {
    close(1);
    close(fds[0]);
    if (dup2(fds[1], 1) < 0) {
      perror("Could not redirect stdout");
      close(fds[1]);
      exit(1);
    }

    close(0);
    if (execvp(argv[0], argv)) {
      perror("Could not execute git");
      close(fds[1]);
      exit(1);
    }
  }

  close(fds[1]);
  f = fdopen(fds[0], "r");
  for (;;) {
    char buffer[1024 + 42];
    int len, slashes;

    if (!fgets(buffer, sizeof(buffer), f)) {
      break;
    }

    for (len = slashes = 0; buffer[len]; len++) {
      if (buffer[len] == '/') {
        slashes++;
      }
    }

    if (len > 40 && buffer[40] == ' ' && buffer[41] != '.' && slashes == 2 && strcmp(buffer + len - 12, "/.gitignore\n")) {
      buffer[40] = '\0';
      if (hashtable_index(&table, buffer) < 0) {
        add_file_to_list(buffer, msgs);
        add_to_hashtable(&table, buffer);
      }
    }
  }
  free(table.blob_names);
  fclose(f);
  close(fds[0]);
}
/*}}}*/
void copy_gitblob_to_path(char *blob_name, const char *target_path)/*{{{*/
{
  char *argv[7] = {
    "git", "--git-dir", git_dir, "cat-file", "blob", blob_name, NULL
  };
  pid_t subprocess;
  int status;

  if (!git_dir) {
    fprintf(stderr, "No git dir set up");
    exit(1);
  }

  subprocess = fork();
  if (subprocess < 0) {
    fprintf(stderr, "While trying to read %s: ", blob_name);
    perror("Could not fork");
    exit(1);
  }
  if (!subprocess) {
    int fd = open(target_path, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
      perror("Could not copy git blob");
      exit(1);
    }

    close(1);
    if (dup2(fd, 1) < 0) {
      perror("Could not redirect stdout");
      close(fd);
      exit(1);
    }

    close(0);
    if (execvp(argv[0], argv)) {
      perror("Could not execute git");
      exit(1);
    }
  }

  if (waitpid(subprocess, &status, 0) < 0) {
    perror("Could not fork git");
    exit(1);
  }
}
/*}}}*/
unsigned char *read_blob(char *blob_name, int *len)/*{{{*/
{
  char *argv[7] = {
    "git", "--git-dir", git_dir, "cat-file", "blob", blob_name, NULL
  };
  int allocated = 0;
  unsigned char *result = NULL;
  int fds[2];
  pid_t subprocess;

  if (!git_dir) {
    fprintf(stderr, "No git dir set up");
    return NULL;
  }

  if (pipe(fds)) {
    perror("Could not set up a pipe");
    return NULL;
  }

  subprocess = fork();
  if (subprocess < 0) {
    fprintf(stderr, "While trying to read %s into memory: ", blob_name);
    perror("Could not fork");
    exit(1);
  }
  if (!subprocess) {
    close(1);
    close(fds[0]);
    if (dup2(fds[1], 1) < 0) {
      perror("Could not redirect stdout");
      close(fds[1]);
      exit(1);
    }

    close(0);
    if (execvp(argv[0], argv)) {
      perror("Could not execute git");
      close(fds[1]);
      exit(1);
    }
  }

  close(fds[1]);
  *len = 0;
  for (;;) {
    int count;

    if (*len + 1024 > allocated) {
      allocated = *len + 1024;
      result = grow_array(unsigned char, allocated, result);
    }
    count = read(fds[0], result + *len, 1024);
    if (!count) {
      break;
    }
    if (count < 0) {
      perror("Error while reading blob");
      return NULL;
    }
    *len += count;
  }
  close(fds[0]);

  return result;
}
/*}}}*/
struct rfc822 *make_rfc822_git(char *blob_name)/*{{{*/
{
  struct rfc822 *result = NULL;
  int len;
  unsigned char *data = read_blob(blob_name, &len);

  if (data) {
    result = make_rfc822_data(blob_name, (char *) data, len);
    free(data);
  }
  else {
    fprintf(stderr, "Could not read blob %s\n", blob_name);
  }

  return result;
}
/*}}}*/
