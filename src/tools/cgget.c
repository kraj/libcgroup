// SPDX-License-Identifier: LGPL-2.1-only
#include "tools-common.h"

#include <libcgroup.h>
#include <libcgroup-internal.h>

#include <dirent.h>
#include <getopt.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define MODE_SHOW_HEADERS	1
#define MODE_SHOW_NAMES		2

#define LL_MAX			100

static const struct option long_options[] = {
	{"variable",	required_argument, NULL, 'r'},
	{"help",	      no_argument, NULL, 'h'},
	{"all",		      no_argument, NULL, 'a'},
	{"values-only",	      no_argument, NULL, 'v'},
	{NULL, 0, NULL, 0}
};

static void usage(int status, const char *program_name)
{
	if (status != 0) {
		err("Wrong input parameters, try %s -h' for more information.\n", program_name);
		return;
	}
	info("Usage: %s [-nv] [-r <name>] [-g <controllers>] [-a] <path> ...\n", program_name);
	info("Print parameter(s) of given group(s).\n");
	info("  -a, --all			Print info about all relevant controllers\n");
	info("  -g <controllers>		Controller which info should be displayed\n");
	info("  -g <controllers>:<path>	Control group which info should be displayed\n");
	info("  -h, --help			Display this help\n");
	info("  -n				Do not print headers\n");
	info("  -r, --variable <name>		Define parameter to display\n");
	info("  -v, --values-only		Print only values, not ");
	info("parameter names\n");
}

static int get_controller_from_name(const char * const name, char **controller)
{
	char *dot;

	*controller = strdup(name);
	if (*controller == NULL)
		return ECGOTHER;

	dot = strchr(*controller, '.');
	if (dot == NULL) {
		err("cgget: error parsing parameter name '%s'", name);
		return ECGINVAL;
	}
	*dot = '\0';

	return 0;
}

static int create_cg(struct cgroup **cg_list[], int * const cg_list_len)
{
	*cg_list = realloc(*cg_list, ((*cg_list_len) + 1) * sizeof(struct cgroup *));
	if ((*cg_list) == NULL)
		return ECGCONTROLLERCREATEFAILED;

	memset(&(*cg_list)[*cg_list_len], 0, sizeof(struct cgroup *));

	(*cg_list)[*cg_list_len] = cgroup_new_cgroup("");
	if ((*cg_list)[*cg_list_len] == NULL)
		return ECGCONTROLLERCREATEFAILED;

	(*cg_list_len)++;

	return 0;
}

static int parse_a_flag(struct cgroup **cg_list[], int * const cg_list_len)
{
	struct cgroup_mount_point controller;
	struct cgroup_controller *cgc;
	struct cgroup *cg = NULL;
	void *handle;
	int ret = 0;

	if ((*cg_list_len) == 0) {
		ret = create_cg(cg_list, cg_list_len);
		if (ret)
			goto out;
	}

	/*
	 * if "-r" was provided, then we know that the cgroup(s) will be
	 * an optarg at the end with no flag.  Let's temporarily populate
	 * the first cgroup with the requested control values.
	 */
	cg = (*cg_list)[0];

	ret = cgroup_get_controller_begin(&handle, &controller);
	while (ret == 0) {
		cgc = cgroup_get_controller(cg, controller.name);
		if (!cgc) {
			cgc = cgroup_add_controller(cg, controller.name);
			if (!cgc) {
				err("cgget: cannot find controller '%s'\n", controller.name);
				ret = ECGOTHER;
				goto out;
			}
		}
		ret = cgroup_get_controller_next(&handle, &controller);
	}
	if (ret == ECGEOF)
		/*
		 * we successfully reached the end of the controller list;
		 * this is not an error
		 */
		ret = 0;

	cgroup_get_controller_end(&handle);

	return ret;

out:
	cgroup_get_controller_end(&handle);

	return ret;
}

static int parse_r_flag(struct cgroup **cg_list[], int * const cg_list_len,
			const char * const cntl_value)
{
	char *cntl_value_controller = NULL;
	struct cgroup_controller *cgc;
	struct cgroup *cg = NULL;
	int ret = 0;

	if ((*cg_list_len) == 0) {
		ret = create_cg(cg_list, cg_list_len);
		if (ret)
			goto out;
	}

	/*
	 * if "-r" was provided, then we know that the cgroup(s) will be
	 * an optarg at the end with no flag.  Let's temporarily populate
	 * the first cgroup with the requested control values.
	 */
	cg = (*cg_list)[0];

	ret = get_controller_from_name(cntl_value, &cntl_value_controller);
	if (ret)
		goto out;

	cgc = cgroup_get_controller(cg, cntl_value_controller);
	if (!cgc) {
		cgc = cgroup_add_controller(cg, cntl_value_controller);
		if (!cgc) {
			err("cgget: cannot find controller '%s'\n", cntl_value_controller);
			ret = ECGOTHER;
			goto out;
		}
	}

	ret = cgroup_add_value_string(cgc, cntl_value, NULL);

out:
	if (cntl_value_controller)
		free(cntl_value_controller);

	return ret;
}

static int parse_g_flag_no_colon(struct cgroup **cg_list[], int * const cg_list_len,
				 const char * const ctrl_str)
{
	struct cgroup_controller *cgc;
	struct cgroup *cg = NULL;
	int ret = 0;

	if ((*cg_list_len) > 1) {
		ret = ECGMAXVALUESEXCEEDED;
		goto out;
	}

	if ((*cg_list_len) == 0) {
		ret = create_cg(cg_list, cg_list_len);
		if (ret)
			goto out;
	}

	/*
	 * if "-g <controller>" was provided, then we know that the cgroup(s)
	 * will be an optarg at the end with no flag.  Let's temporarily
	 * populate the first cgroup with the requested control values.
	 */
	cg = *cg_list[0];

	cgc = cgroup_get_controller(cg, ctrl_str);
	if (!cgc) {
		cgc = cgroup_add_controller(cg, ctrl_str);
		if (!cgc) {
			err("cgget: cannot find controller '%s'\n", ctrl_str);
			ret = ECGOTHER;
			goto out;
		}
	}

out:
	return ret;
}

static int split_cgroup_name(const char * const ctrl_str, char *cg_name)
{
	char *colon;

	colon = strchr(ctrl_str, ':');
	if (colon == NULL) {
		/* ctrl_str doesn't contain a ":" */
		cg_name[0] = '\0';
		return ECGINVAL;
	}

	strncpy(cg_name, &colon[1], FILENAME_MAX - 1);

	return 0;
}

static int split_controllers(const char * const in, char **ctrl[], int * const ctrl_len)
{
	char *copy, *tok, *colon, *saveptr = NULL;
	int ret = 0;
	char **tmp;

	copy = strdup(in);
	if (!copy)
		goto out;
	saveptr = copy;

	colon = strchr(copy, ':');
	if (colon)
		colon[0] = '\0';

	while ((tok = strtok_r(copy, ",", &copy))) {
		tmp = realloc(*ctrl, sizeof(char *) * ((*ctrl_len) + 1));
		if (!tmp) {
			ret = ECGOTHER;
			goto out;
		}

		*ctrl = tmp;
		(*ctrl)[*ctrl_len] = strdup(tok);
		if ((*ctrl)[*ctrl_len] == NULL) {
			ret = ECGOTHER;
			goto out;
		}

		(*ctrl_len)++;
	}

out:
	if (saveptr)
		free(saveptr);

	return ret;
}

static int parse_g_flag_with_colon(struct cgroup **cg_list[], int * const cg_list_len,
				   const char * const ctrl_str)
{
	struct cgroup_controller *cgc;
	struct cgroup *cg = NULL;
	char **controllers = NULL;
	int controllers_len = 0;
	int i, ret = 0;

	ret = create_cg(cg_list, cg_list_len);
	if (ret)
		goto out;

	cg = (*cg_list)[(*cg_list_len) - 1];

	ret = split_cgroup_name(ctrl_str, cg->name);
	if (ret)
		goto out;

	ret = split_controllers(ctrl_str, &controllers, &controllers_len);
	if (ret)
		goto out;

	for (i = 0; i < controllers_len; i++) {
		cgc = cgroup_get_controller(cg, controllers[i]);
		if (!cgc) {
			cgc = cgroup_add_controller(cg, controllers[i]);
			if (!cgc) {
				err("cgget: cannot find controller '%s'\n", controllers[i]);
				ret = ECGOTHER;
				goto out;
			}
		}
	}

out:
	for (i = 0; i < controllers_len; i++)
		free(controllers[i]);

	return ret;
}

static int parse_opt_args(int argc, char *argv[], struct cgroup **cg_list[],
			  int * const cg_list_len, bool first_cg_is_dummy)
{
	struct cgroup *cg = NULL;
	int ret = 0;

	/*
	 * The first cgroup was temporarily populated and requires the
	 * user to provide a cgroup name as an opt
	 */
	if (argv[optind] == NULL && first_cg_is_dummy) {
		usage(1, argv[0]);
		exit(-1);
	}

	/*
	 * The user has provided a cgroup and controller via the
	 * -g <controller>:<cgroup> flag and has also provided a cgroup
	 *  via the optind.  This was not supported by the previous cgget
	 * implementation.  Continue that approach.
	 *
	 * Example of a command that will hit this code:
	 *	$ cgget -g cpu:mycgroup mycgroup
	 */
	if (argv[optind] != NULL && (*cg_list_len) > 0 &&
	    strcmp((*cg_list)[0]->name, "") != 0) {
		usage(1, argv[0]);
		exit(-1);
	}

	while (argv[optind] != NULL) {
		if ((*cg_list_len) > 0)
			cg = (*cg_list)[(*cg_list_len) - 1];
		else
			cg = NULL;

		if ((*cg_list_len) == 0) {
			/*
			 * The user didn't provide a '-r' or '-g' flag.
			 * The parse_a_flag() function can be reused here
			 * because we both have the same use case - gather
			 * all the data about this particular cgroup.
			 */
			ret = parse_a_flag(cg_list, cg_list_len);
			if (ret)
				goto out;

			strncpy((*cg_list)[(*cg_list_len) - 1]->name, argv[optind],
				sizeof((*cg_list)[(*cg_list_len) - 1]->name) - 1);
		} else if (cg != NULL && strlen(cg->name) == 0) {
			/*
			 * this cgroup was created based upon control/value
			 * pairs or with a -g <controller> option.  we'll
			 * populate it with the parameter provided by the user
			 */
			strncpy(cg->name, argv[optind], sizeof(cg->name) - 1);
		} else {
			ret = create_cg(cg_list, cg_list_len);
			if (ret)
				goto out;

			ret = cgroup_copy_cgroup((*cg_list)[(*cg_list_len) - 1],
						 (*cg_list)[(*cg_list_len) - 2]);
			if (ret)
				goto out;

			strncpy((*cg_list)[(*cg_list_len) - 1]->name, argv[optind],
				sizeof((*cg_list)[(*cg_list_len) - 1]->name) - 1);
		}
		optind++;
	}

out:
	return ret;
}

static int parse_opts(int argc, char *argv[], struct cgroup **cg_list[], int * const cg_list_len,
		      int * const mode)
{
	bool do_not_fill_controller = false;
	bool first_cgroup_is_dummy = false;
	bool fill_controller = false;
	int ret = 0;
	int c;

	/* Parse arguments. */
	while ((c = getopt_long(argc, argv, "r:hnvg:a", long_options, NULL)) > 0) {
		switch (c) {
		case 'h':
			usage(0, argv[0]);
			exit(0);
		case 'n':
			/* Do not show headers. */
			*mode = (*mode) & (INT_MAX ^ MODE_SHOW_HEADERS);
			break;
		case 'v':
			/* Do not show parameter names. */
			*mode = (*mode) & (INT_MAX ^ MODE_SHOW_NAMES);
			break;
		case 'r':
			do_not_fill_controller = true;
			first_cgroup_is_dummy = true;
			ret = parse_r_flag(cg_list, cg_list_len, optarg);
			if (ret)
				goto err;
			break;
		case 'g':
			fill_controller = true;
			if (strchr(optarg, ':') == NULL) {
				first_cgroup_is_dummy = true;
				ret = parse_g_flag_no_colon(cg_list, cg_list_len, optarg);
				if (ret)
					goto err;
			} else {
				ret = parse_g_flag_with_colon(cg_list, cg_list_len, optarg);
				if (ret)
					goto err;
			}
			break;
		case 'a':
			fill_controller = true;
			ret = parse_a_flag(cg_list, cg_list_len);
			if (ret)
				goto err;
			break;
		default:
			usage(1, argv[0]);
			exit(1);
		}
	}

	/* Don't allow '-r' and ('-g' or '-a') */
	if (fill_controller && do_not_fill_controller) {
		usage(1, argv[0]);
		exit(1);
	}

	ret = parse_opt_args(argc, argv, cg_list, cg_list_len, first_cgroup_is_dummy);
	if (ret)
		goto err;

err:
	return ret;
}

static int get_cv_value(struct control_value * const cv, const char * const cg_name,
			const char * const controller_name)
{
	bool is_multiline = false;
	char tmp_line[LL_MAX];
	void *handle, *tmp;
	int ret;

	ret = cgroup_read_value_begin(controller_name, cg_name, cv->name, &handle, tmp_line,
				      LL_MAX);
	if (ret == ECGEOF)
		goto read_end;
	if (ret != 0) {
		if (ret == ECGOTHER) {
			int tmp_ret;

			/*
			 * to maintain compatibility with an earlier version
			 * of cgget, try to determine if the failure was due
			 * to an invalid controller
			 */
			tmp_ret = cgroup_test_subsys_mounted(controller_name);
			if (tmp_ret == 0) {
				err("cgget: cannot find controller '%s' in group '%s'\n",
				    controller_name, cg_name);
			} else {
				err("variable file read failed %s\n", cgroup_strerror(ret));
			}
		}

		goto end;
	}

	/* remove the newline character */
	tmp_line[strcspn(tmp_line, "\n")] = '\0';

	strncpy(cv->value, tmp_line, CG_CONTROL_VALUE_MAX - 1);
	cv->multiline_value = strdup(cv->value);
	if (cv->multiline_value == NULL)
		goto read_end;

	while ((ret = cgroup_read_value_next(&handle, tmp_line, LL_MAX)) == 0) {
		if (ret == 0) {
			is_multiline = true;
			cv->value[0] = '\0';

			/* remove the newline character */
			tmp_line[strcspn(tmp_line, "\n")] = '\0';

			tmp = realloc(cv->multiline_value, sizeof(char) *
				(strlen(cv->multiline_value) + strlen(tmp_line) + 3));
			if (tmp == NULL)
				goto read_end;

			cv->multiline_value = tmp;
			strcat(cv->multiline_value, "\n\t");
			strcat(cv->multiline_value, tmp_line);
		}
	}

read_end:
	cgroup_read_value_end(&handle);
	if (ret == ECGEOF)
		ret = 0;
end:
	if (is_multiline == false && cv->multiline_value) {
		free(cv->multiline_value);
		cv->multiline_value = NULL;
	}

	if ((FILE *)handle)
		fclose((FILE *)handle);

	return ret;
}

static int indent_multiline_value(struct control_value * const cv)
{
	char tmp_val[CG_CONTROL_VALUE_MAX] = {0};
	char *tok, *saveptr = NULL;

	tok = strtok_r(cv->value, "\n", &saveptr);
	strncat(tmp_val, tok, CG_CONTROL_VALUE_MAX - 1);
	/* don't indent the first value */
	while ((tok = strtok_r(NULL, "\n", &saveptr))) {
		strncat(tmp_val, "\n\t", (CG_CONTROL_VALUE_MAX - (strlen(tmp_val) + 1)));
		strncat(tmp_val, tok, (CG_CONTROL_VALUE_MAX - (strlen(tmp_val) + 1)));
	}

	cv->multiline_value = strdup(tmp_val);
	if (!cv->multiline_value)
		return ECGOTHER;

	return 0;
}

static int fill_empty_controller(struct cgroup * const cg, struct cgroup_controller * const cgc)
{
	struct dirent *ctrl_dir = NULL;
	bool found_mount = false;
	int i, path_len, ret = 0;
	char path[FILENAME_MAX];
	DIR *dir = NULL;

	pthread_rwlock_rdlock(&cg_mount_table_lock);

	for (i = 0; i < CG_CONTROLLER_MAX && cg_mount_table[i].name[0] != '\0'; i++) {
		if (strlen(cgc->name) == strlen(cg_mount_table[i].name) &&
		    strncmp(cgc->name, cg_mount_table[i].name, strlen(cgc->name)) == 0) {
			found_mount = true;
			break;
		}
	}

	if (found_mount == false)
		goto out;

	if (!cg_build_path_locked(NULL, path, cg_mount_table[i].name))
		goto out;

	path_len = strlen(path);
	strncat(path, cg->name, FILENAME_MAX - path_len - 1);
	path[sizeof(path) - 1] = '\0';

	if (access(path, F_OK))
		goto out;

	if (!cg_build_path_locked(cg->name, path, cg_mount_table[i].name))
		goto out;

	dir = opendir(path);
	if (!dir) {
		ret = ECGOTHER;
		goto out;
	}

	while ((ctrl_dir = readdir(dir)) != NULL) {
		/* Skip over non regular files */
		if (ctrl_dir->d_type != DT_REG)
			continue;

		ret = cgroup_fill_cgc(ctrl_dir, cg, cgc, i);
		if (ret == ECGFAIL)
			goto out;

		if (cgc->index > 0) {
			cgc->values[cgc->index - 1]->dirty = false;

			/*
			 * previous versions of cgget indented the second
			 * and all subsequent lines. Continue that behavior
			 */
			if (strchr(cgc->values[cgc->index - 1]->value, '\n')) {
				ret = indent_multiline_value(
					cgc->values[cgc->index - 1]);
				if (ret)
					goto out;
			}
		}
	}

out:
	if (dir)
		closedir(dir);

	pthread_rwlock_unlock(&cg_mount_table_lock);

	return ret;
}

static int get_controller_values(struct cgroup * const cg, struct cgroup_controller * const cgc)
{
	int ret = 0;
	int i;

	for (i = 0; i < cgc->index; i++) {
		ret = get_cv_value(cgc->values[i], cg->name, cgc->name);
		if (ret)
			goto out;
	}

	if (cgc->index == 0) {
		/* fill the entire controller since no values were provided */
		ret = fill_empty_controller(cg, cgc);
		if (ret)
			goto out;
	}

out:
	return ret;
}

static int get_cgroup_values(struct cgroup * const cg)
{
	int ret = 0;
	int i;

	for (i = 0; i < cg->index; i++) {
		ret = get_controller_values(cg, cg->controller[i]);
		if (ret)
			break;
	}

	return ret;
}

static int get_values(struct cgroup *cg_list[], int cg_list_len)
{
	int ret = 0;
	int i;

	for (i = 0; i < cg_list_len; i++) {
		ret = get_cgroup_values(cg_list[i]);
		if (ret)
			break;
	}

	return ret;
}

void print_control_values(const struct control_value * const cv, int mode)
{
	if (mode & MODE_SHOW_NAMES)
		info("%s: ", cv->name);

	if (cv->multiline_value)
		info("%s\n", cv->multiline_value);
	else
		info("%s\n", cv->value);
}

void print_controller(const struct cgroup_controller * const cgc, int mode)
{
	int i;

	for (i = 0; i < cgc->index; i++)
		print_control_values(cgc->values[i], mode);
}

static void print_cgroup(const struct cgroup * const cg, int mode)
{
	int i;

	if (mode & MODE_SHOW_HEADERS)
		info("%s:\n", cg->name);

	for (i = 0; i < cg->index; i++)
		print_controller(cg->controller[i], mode);

	if (mode & MODE_SHOW_HEADERS)
		info("\n");
}

static void print_cgroups(struct cgroup *cg_list[], int cg_list_len, int mode)
{
	int i;

	for (i = 0; i < cg_list_len; i++)
		print_cgroup(cg_list[i], mode);
}

int main(int argc, char *argv[])
{
	int mode = MODE_SHOW_NAMES | MODE_SHOW_HEADERS;
	struct cgroup **cg_list = NULL;
	int cg_list_len = 0;
	int ret = 0, i;

	/* No parameter on input? */
	if (argc < 2) {
		usage(1, argv[0]);
		return 1;
	}

	ret = cgroup_init();
	if (ret) {
		err("%s: libcgroup initialization failed: %s\n", argv[0], cgroup_strerror(ret));
		goto err;
	}

	ret = parse_opts(argc, argv, &cg_list, &cg_list_len, &mode);
	if (ret)
		goto err;

	ret = get_values(cg_list, cg_list_len);
	if (ret)
		goto err;

	print_cgroups(cg_list, cg_list_len, mode);

err:
	for (i = 0; i < cg_list_len; i++)
		cgroup_free(&(cg_list[i]));

	return ret;
}
