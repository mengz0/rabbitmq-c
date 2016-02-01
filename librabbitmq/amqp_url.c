/* vim:set ft=c ts=2 sw=2 sts=2 et cindent: */
/*
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MIT
 *
 * Portions created by Alan Antonuk are Copyright (c) 2012-2013
 * Alan Antonuk. All Rights Reserved.
 *
 * Portions created by VMware are Copyright (c) 2007-2012 VMware, Inc.
 * All Rights Reserved.
 *
 * Portions created by Tony Garnock-Jones are Copyright (c) 2009-2010
 * VMware, Inc. and Tony Garnock-Jones. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ***** END LICENSE BLOCK *****
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _MSC_VER
# define _CRT_SECURE_NO_WARNINGS
#endif

#include "amqp_private.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TRUE     1
#define FALSE    0

void
array_init(array_t *array, unsigned n, unsigned size,free_elt_func_t free_func)
{
        array->nelts = 0;
        array->size = size;
        array->nalloc = n;
        array->free = free_func;
        array->elts = calloc(size, n);
}


void
array_destroy(array_t *a)
{
	unsigned i = 0;
        if(a->free){
                for(; i < a->nelts; ++i)
                        (*a->free)((unsigned char *)a->elts + i * a->size);
        }

        free(a->elts);
}


void *
array_push(array_t *a)
{
        void        *elt;

        if (a->nelts == a->nalloc) {
                a->nalloc <<= 1;
                a->elts = realloc(a->elts,a->size * a->nalloc);
        }

        elt = (unsigned char *) a->elts + a->size * a->nelts;
        a->nelts++;
        return elt;
}


static void
free_host_port(void *v)
{
	host_port_t *hp = v;
	free(hp->host);
}

void
free_amqp_uri(struct amqp_uri *uri)
{
	if(!uri)
		return;

	free(uri->user);
	free(uri->password);
	array_destroy(&uri->host_port_array);
	free(uri->vhost);
	free(uri->cacert_file);
	free(uri->key_file);
	free(uri->cert_file);
	free(uri->exchange);
	free(uri->queue_name);
	free(uri);
}

amqp_uri_t *
alloc_amqp_uri(void)
{
	amqp_uri_t *uri = calloc(1,sizeof(amqp_uri_t));
	array_init(&uri->host_port_array,1,sizeof(host_port_t),free_host_port);
	uri->heartbeat_interval = 1;
	uri->switch_delay = 1;
	uri->connect_timeout = 5;
	return uri;
}

static char *
__amqp_uri_unescape (const char *string)
{
	array_t array;
	char *c = NULL;
	unsigned int hex = 0;
	const char *curr = NULL, *end = NULL;
	size_t len;

	if(!string)
		return NULL;

	len = strlen(string);
	curr = string;
	end = curr + len;
	array_init(&array,len,sizeof(char),NULL);
	for (; *curr; ++curr) {

		switch (*curr) {
		case '%':
			if (((end - curr) < 2) ||
				!isxdigit(curr[1]) ||
				!isxdigit(curr[2]) ||
				(1 != sscanf(&curr[1], "%02x", &hex)) ||
				!isprint(hex)) {
				array_destroy(&array);
				return NULL;
			}
			c = array_push(&array);
			*c = hex;
			curr += 2;
			break;
		default:
			c = array_push(&array);
			*c = *curr;
			break;
		}
	}

	c = array_push(&array);
	*c = '\0';
	return array.elts;
}


static void
amqp_uri_unescape (char **str)
{
	if(!str)
		return;

	char *tmp = *str;
	if(!tmp)
		return;

	//need unescape?
	if (strchr(tmp,'%')) {
		*str = __amqp_uri_unescape(tmp);
		free(tmp);
	}
}

static char *
seek_stop (const char *str, char stop, const char **end)
{
	const char *it = str;
	if(!it)
		return NULL;

	for ( ; *it; ++it)
		if (*it == stop) {
			*end = it;
			return strndup(str, it - str);
		}
	return NULL;
}


static int
amqp_uri_scheme (amqp_uri_t *uri, const char *str, const char **end)
{

	if (!strncmp(str, "amqp://", 7)){
		*end = str + 7;
		uri->ssl = 0;
		return TRUE;
	}
	
	if(!strncmp(str,"amqps://",8)) {
		*end = str + 8;
		uri->ssl = 1;
		return TRUE;
	}

	return FALSE;
}


static void
amqp_uri_userpass (amqp_uri_t *uri, const char *str, const char **end)
{
	const char *end_userpass;
	const char *end_user;
	char *s;

	if ((s = seek_stop(str, '@', &end_userpass))) {
		if ((uri->user = seek_stop(s, ':', &end_user))) {
			uri->password = strdup(end_user + 1);
		} else {
			uri->user = strndup(str, end_userpass - str);
			uri->password = NULL;
		}
		amqp_uri_unescape(&uri->user);
		amqp_uri_unescape(&uri->password);
		*end = end_userpass + 1;
		free(s);
	} else {
		uri->user     = strdup("guest");
		uri->password = strdup("guest");
	}
}

static int
amqp_uri_host (amqp_uri_t *uri, const char *str)
{
	unsigned short port;
	const char *end_host;
	char *hostname = NULL;

	if ((hostname = seek_stop(str, ':', &end_host))) {
		++end_host;
		if (!isdigit(*end_host)) {
			free(hostname);
			return FALSE;
		}
		sscanf (end_host, "%hu", &port);
	} else {
		hostname = strdup(str);
		port = uri->ssl ? AMQP_DEFAULT_SSL_PORT : AMQP_DEFAULT_PORT;
	}

	amqp_uri_unescape(&hostname);
	host_port_t *hp = array_push(&uri->host_port_array);
	hp->host = hostname;
	hp->port = port;
	return TRUE;
}

static int
amqp_uri_hosts (amqp_uri_t *uri, const char *str,const char **end)
{
	int result = FALSE;
	const char *end_hostport;
	char *s;

loop__:
	if ((s = seek_stop(str, ',', &end_hostport))) {
		if (!amqp_uri_host(uri, s)) {
			free(s);
			return FALSE;
		}
		free(s);
		str = end_hostport + 1;
		result = TRUE;
		goto loop__;
	} else if ((s = seek_stop(str, '/', &end_hostport)) ||
		(s = seek_stop(str, '?', &end_hostport))) {
		if (!amqp_uri_host(uri, s)) {
			free(s);
			return FALSE;
		}
		free(s);
		*end = end_hostport;
		return TRUE;
	} else if (*str) {
		if (!amqp_uri_host(uri, str)) {
			return FALSE;
		}
		*end = str + strlen(str);
		return TRUE;
	}

	return result;
}


static int
amqp_uri_option (amqp_uri_t *uri,const char *str)
{
	const char *end_key;
	char *key;
	char *value;

	if (!(key = seek_stop(str, '=', &end_key))) {
		return FALSE;
	}

	int result = TRUE;
	value = strdup(end_key + 1);
	amqp_uri_unescape(&value);
	if (!strcasecmp(key, "cacertfile")){
		uri->cacert_file = value;  
	}else if(!strcasecmp(key, "keyfile")){
		uri->key_file = value;
	}else if(!strcasecmp(key, "certfile")){
		uri->cert_file = value;
	}else if(!strcasecmp(key, "exchange")){
		uri->exchange = value;
	}else if(!strcasecmp(key, "queuename")){
		uri->queue_name = value;
	}else if(!strcasecmp(key, "heartbeat")){
		uri->heartbeat_interval = atoi(value);
		free(value);
	}else if(!strcasecmp(key, "switch_delay")){
		uri->switch_delay = atoi(value);
		free(value);
	}else if(!strcasecmp(key, "connect_timeout")){
		uri->connect_timeout = atoi(value);
		free(value);
	}else if(!strcasecmp(key, "ssl_verify")){
		uri->ssl_verify = !strcasecmp(value,"on");
		free(value);
	}else {
		free(value);
		result = FALSE;
	}
	free(key);
	return result;
}


static int
amqp_uri_options (amqp_uri_t *uri, const char *str)
{
	const char *end_option;
	char *option;
        while ((option = seek_stop(str, '&', &end_option))) {
		if (!amqp_uri_option(uri, option)) {
			free(option);
			return FALSE;
		}
		free(option);
		str = end_option + 1;
	}

	if (*str && !amqp_uri_option(uri, str)) {
		return FALSE;
	}
	return TRUE;
}

static int
amqp_uri_vhost(amqp_uri_t *uri,const char *str,const char **end)
{
	const char *end_vhost;
	if ((uri->vhost = seek_stop(str, '?', &end_vhost))) {
		*end = end_vhost;
	} else if (*str) {
		uri->vhost = strdup(str);
		*end = str + strlen(str);
	}
	amqp_uri_unescape(&uri->vhost);
	return TRUE;
}

int
amqp_uri_parse (amqp_uri_t *uri, const char *str)
{
	if(!uri)
		return FALSE;

	//1. scheme
	if (!amqp_uri_scheme(uri, str, &str)) {
		return FALSE;
	}

	if (!*str) {
		return FALSE;
	}

	//2. username/password
	amqp_uri_userpass(uri, str, &str);

	//3. hosts
	if (!*str || !amqp_uri_hosts(uri, str, &str)) {
		return FALSE;
	}

	switch (*str) {
	case '/':
		//4. vhost
		if (*str && !amqp_uri_vhost(uri, str, &str)) {
			return FALSE;
		}
		if (!*str) {
			break;
		}
		/* fall through */
	case '?':
		str++;
		//5. options
		if (*str && !amqp_uri_options(uri, str)) {
			return FALSE;
		}
		break;
	default:
		break;
	}

	return TRUE;
}


