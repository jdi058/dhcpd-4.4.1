/* mdb.c

   Server-specific in-memory database support. */

/*
 * Copyright (c) 2004-2017 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1996-2003 by Internet Software Consortium
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *   Internet Systems Consortium, Inc.
 *   950 Charter Street
 *   Redwood City, CA 94063
 *   <info@isc.org>
 *   https://www.isc.org/
 *
 */

#include "dhcpd.h"
#include "omapip/hash.h"

struct subnet *subnets;
struct shared_network *shared_networks;
host_hash_t *host_hw_addr_hash;
host_hash_t *host_uid_hash;
host_hash_t *host_name_hash;
lease_id_hash_t *lease_uid_hash;
lease_ip_hash_t *lease_ip_addr_hash;
lease_id_hash_t *lease_hw_addr_hash;

/*
 * We allow users to specify any option as a host identifier.
 *
 * Any host is uniquely identified by the combination of 
 * option type & option data.
 *
 * We expect people will only use a few types of options as host 
 * identifier. Because of this, we store a list with an entry for
 * each option type. Each of these has a hash table, which contains 
 * hash of the option data.
 *
 * For v6 we also include a relay count - this specifies which
 * relay to check for the requested option.  As each different
 * value of relays creates a new instance admins should use the
 * same value across each option for all host-identifers.
 * A value of 0 indicates that we aren't doing relay options
 * and should simply look in the current option list.
 */
 /* 我们允许user指定任何option为主机标识符，任何主机都被option类型和option数据两个部分
 混合而成，我们希望用户使用尽量少的options作为主机标识符，因此，给每个option类型存储一
 个hash表，每个都包含options数据 */
 
typedef struct host_id_info 
{
	struct option *option;
	host_hash_t *values_hash;
	int relays;
	struct host_id_info *next;
} host_id_info_t;

static host_id_info_t *host_id_info = NULL;

int numclasseswritten;

omapi_object_type_t *dhcp_type_host;

/*********************************************************************
Func Name :   enter_class
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  加入一个class
Input:	      IN struct class * cd
			  IN int dynamicp
			  IN int commit
Output:       
Return:       int
Caution : 	  
*********************************************************************/
isc_result_t enter_class
(
	struct class * cd, 
	int dynamicp, 
	int commit
)
{
	/* 若collections下面没有classes，检查cd->name，将cd挂接到classes上 */
	if (!collections->classes) 
	{
		/* A subclass with no parent is invalid. */
		if (cd->name == NULL)
			return DHCP_R_INVALIDARG;

		class_reference(&collections->classes, cd, MDL);
	} 
	else if (cd->name != NULL) 
	{	/* regular class */
		struct class *c = 0;

		/* 调用class.c中的find_class */
		/* 若找到了，说明cd已经在链表中了，直接return */
		if (find_class(&c, cd->name, MDL) != ISC_R_NOTFOUND) 
		{
			class_dereference(&c, MDL);
			return ISC_R_EXISTS;
		}
		
		/* 将cd挂接到链表尾部 */
		for (c = collections->classes; c->nic; c = c->nic);
		class_reference(&c->nic, cd, MDL);
	}

	if (dynamicp && commit) 
	{
		const char *name = cd->name;

		if (name == NULL) 
		{
			name = cd->superclass->name;
		}

		write_named_billing_class((const unsigned char *)name, 0, cd);
		if (!commit_leases())
			return ISC_R_IOERROR;
	}

	return ISC_R_SUCCESS;
}


/* Variable to check if we're starting the server.  The server will init as
 * starting - but just to be safe start out as false to avoid triggering new
 * special-case code
 * XXX: There is actually a server_startup state...which is never entered...
 */
#define SS_NOSYNC	1
#define SS_QFOLLOW	2
static int server_starting = 0;

/*********************************************************************
Func Name :   find_uid_statement
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  
Input:	      IN struct executable_statement * esp
			  IN void * vp
			  IN int condp
Output:       
Return:       int
Caution : 	  
*********************************************************************/
static int find_uid_statement 
(
	struct executable_statement *esp,
	void *vp, 
	int condp
)
{
	struct executable_statement **evp = vp;

	if (esp->op == supersede_option_statement &&
	    esp->data.option &&
	    (esp->data.option->option->universe == &dhcp_universe) &&
	    (esp->data.option->option->code ==  DHO_DHCP_CLIENT_IDENTIFIER)) 
	{
		if (condp) 
		{
			log_error("dhcp client identifier may not be %s",
				   "specified conditionally.");
		} 
		/* 若没挂接，将esp挂接到vp */
		else if (!(*evp)) 
		{
			executable_statement_reference(evp, esp, MDL);
			return 1;
		} 
		else 
		{
			log_error("only one dhcp client identifier may be %s", "specified");
		}
	}
	return 0;
}

/*********************************************************************
Func Name :   find_host_id_info
Date Created: 2018/05/21
Author:  	  wangzhe
Description:  从host_id_info链表中寻找和option_code和relays匹配的
Input:	      IN unsigned int option_code
			  IN int relays
Output:       
Return:       static host_id_info_t *
Caution : 	  若没找到返回NULL
*********************************************************************/
static host_id_info_t * find_host_id_info
(
	unsigned int option_code, 
	int relays
) 
{
	host_id_info_t *p;

	for (p = host_id_info; p != NULL; p = p->next) 
	{
		if ((p->option->code == option_code) && (p->relays == relays)) 
		{
			break;
		}
	}
	return p;
}

/* Debugging code */
#if 0
isc_result_t
print_host(const void *name, unsigned len, void *value) {
	struct host_decl *h;
	printf("--------------\n");
	printf("name:'%s'\n", print_hex_1(len, name, 60));
	printf("len:%d\n", len);
	h = (struct host_decl *)value;
	printf("host @%p is '%s'\n", h, h->name);
	return ISC_R_SUCCESS;
}

void
hash_print_hosts(struct hash_table *h) {
	hash_foreach(h, print_host);
	printf("--------------\n");
}
#endif /* 0 */

/*********************************************************************
Func Name :   change_host_uid
Date Created: 2018/06/01
Author:  	  wangzhe
Description:  将host的uid设置为指定的值并加入hash
Input:	      IN unsigned int option_code
			  IN int relays
Output:       
Return:       static host_id_info_t *
Caution : 	  若没找到返回NULL
*********************************************************************/
void change_host_uid
(
	struct host_decl *host, 
	const char *uid, 
	int len
) 
{
	/* 若host_uid_hash为空，新创建一个hash表 */
	/* XXX: should consolidate this type of code throughout */
	if (host_uid_hash == NULL) 
	{
		/* 调用new_hash，使用do_string_hash */
		if (!host_new_hash(&host_uid_hash, HOST_HASH_SIZE, MDL)) 
		{
			log_fatal("Can't allocate host/uid hash");
		}
	}

	/* Remove the old entry, if one exists. */
	if (host->client_identifier.data != NULL) 
	{
		host_hash_delete(host_uid_hash,
				 host->client_identifier.data,
				 host->client_identifier.len,
				 MDL);
		data_string_forget(&host->client_identifier, MDL);
	}

	/* 
	 * Set our new value.
	 */
	memset(&host->client_identifier, 0, sizeof(host->client_identifier));
	host->client_identifier.len = len;
	
	if (!buffer_allocate(&host->client_identifier.buffer, len, MDL)) 
	{
		log_fatal("Can't allocate uid buffer");
	}
	host->client_identifier.data = host->client_identifier.buffer->data;
	memcpy((char *)host->client_identifier.data, uid, len);

	/* 以uid为key加入hash表 */
	host_hash_add(host_uid_hash, host->client_identifier.data,  host->client_identifier.len, host, MDL);
}

/*********************************************************************
Func Name :   enter_host
Date Created: 2018/06/28
Author:  	  wangzhe
Description:  保存host结构，解析conf文件遇到固定IP地址的时候进入这个函数保存信息
Input:	      IN struct host_decl * hd	主机结构
			  IN int dynamicp			是否是固定地址
			  IN int commit				是否写入到文件
Output:       
Return:       
Caution : 	  
*********************************************************************/
isc_result_t enter_host
(
	struct host_decl * hd, 
	int dynamicp, 
	int commit
)
{
	struct host_decl *hp = (struct host_decl *)0;
	struct host_decl *np = (struct host_decl *)0;
	struct executable_statement *esp;
	host_id_info_t *h_id_info;

	/* 找name的hash表是否存在，若不存在，重新创建 */
	if (!host_name_hash) 
	{
		if (!host_new_hash(&host_name_hash, HOST_HASH_SIZE, MDL))
			log_fatal ("Can't allocate host name hash");
		host_hash_add(host_name_hash, (unsigned char *)hd->name,
			       strlen (hd -> name), hd, MDL);
	} 
	/* 若name的hash表已经存在，用hd->name为key找hd */
	else 
	{
		host_hash_lookup(&hp, host_name_hash, (unsigned char *)hd->name,
				  strlen (hd->name), MDL);

		/* If it's deleted, we can supersede it. */
		if (hp && (hp->flags & HOST_DECL_DELETED)) 
		{
			host_hash_delete(host_name_hash, (unsigned char *)hd->name, strlen(hd->name), MDL);
			/* If the old entry wasn't dynamic, then we always have to keep the deletion. */
			if (hp->flags & HOST_DECL_STATIC) 
			{
				hd->flags |= HOST_DECL_STATIC;
			}
			host_dereference(&hp, MDL);
		}

		/* If we are updating an existing host declaration, we
		   can just delete it and add it again. */
		if (hp && hp == hd) 
		{
			host_dereference(&hp, MDL);
			delete_host(hd, 0);
			if (!write_host(hd))
				return ISC_R_IOERROR;
			hd->flags &= ~HOST_DECL_DELETED;
		}

		/* If there isn't already a host decl matching this
		   address, add it to the hash table. */
		if (!hp) 
		{
			host_hash_add(host_name_hash,
				       (unsigned char *)hd->name,
				       strlen (hd->name), hd, MDL);
		} 
		else 
		{
			/* XXX actually, we have to delete the old one
			   XXX carefully and replace it.   Not done yet. */
			host_dereference(&hp, MDL);
			return ISC_R_EXISTS;
		}
	}

	if (hd->n_ipaddr)
		host_dereference(&hd->n_ipaddr, MDL);

	if (!hd->type)
		hd->type = dhcp_type_host;

	if (hd->interface.hlen) 
	{
		if (!host_hw_addr_hash) 
		{
			if (!host_new_hash(&host_hw_addr_hash, HOST_HASH_SIZE, MDL))
				log_fatal ("Can't allocate host/hw hash");
		} 
		else 
		{
			/* If there isn't already a host decl matching this
			   address, add it to the hash table. */
			host_hash_lookup(&hp, host_hw_addr_hash, hd->interface.hbuf,
					  hd->interface.hlen, MDL);
		}
		
		if (!hp)
		{
			host_hash_add (host_hw_addr_hash, hd->interface.hbuf,
				       hd->interface.hlen, hd, MDL);
		}
		/* 加到hash桶拉出来的链表的最后 */
		else 
		{
			/* If there was already a host declaration for
			   this hardware address, add this one to the
			   end of the list. */
			for (np = hp; np->n_ipaddr; np = np->n_ipaddr);
			host_reference(&np->n_ipaddr, hd, MDL);
			host_dereference(&hp, MDL);
		}
	}

	/* See if there's a statement that sets the client identifier.
	   This is a kludge - the client identifier really shouldn't be
	   set with an executable statement. */
	esp = NULL;
	if (executable_statement_foreach(hd->group->statements, find_uid_statement, &esp, 0)) 
	{
		(void)evaluate_option_cache(&hd->client_identifier,
					      NULL, NULL, NULL, NULL, NULL, 
					      &global_scope,
					      esp->data.option, MDL);
	}

	/* If we got a client identifier, hash this entry by client identifier. */
	if (hd->client_identifier.len) 
	{
		/* If there's no uid hash, make one; otherwise, see if
		   there's already an entry in the hash for this host. */
		if (!host_uid_hash) 
		{
			if (!host_new_hash(&host_uid_hash, HOST_HASH_SIZE, MDL))
				log_fatal("Can't allocate host/uid hash");

			host_hash_add(host_uid_hash,
				       hd->client_identifier.data,
				       hd->client_identifier.len,
				       hd, MDL);
		} 
		else 
		{
			/* If there's already a host declaration for this
			   client identifier, add this one to the end of the
			   list.  Otherwise, add it to the hash table. */
			if (host_hash_lookup (&hp, host_uid_hash,
					      hd->client_identifier.data,
					      hd->client_identifier.len,
					      MDL)) {
				/* Don't link it in twice... */
				if (!np) 
				{
					for (np = hp; np->n_ipaddr; np = np->n_ipaddr) 
					{
						if (hd == np)
						    break;
					}
					if (hd != np)
					    host_reference(&np->n_ipaddr, hd, MDL);
				}
				host_dereference (&hp, MDL);
			} 
			else 
			{
				host_hash_add(host_uid_hash,
					       hd->client_identifier.data,
					       hd->client_identifier.len,
					       hd, MDL);
			}
		}
	}


	/* If we use an option as our host identifier, record it here.*/
	if (hd->host_id_option != NULL) 
	{
		/* Look for the host identifier information for this option,
		 * and create a new entry if there is none.*/
		h_id_info = find_host_id_info(hd->host_id_option->code, hd->relays);
		if (h_id_info == NULL) 
		{
			h_id_info = dmalloc(sizeof(*h_id_info), MDL);
			if (h_id_info == NULL) 
			{
				log_fatal("No memory for host-identifier "
					  "option information.");
			}
			
			option_reference(&h_id_info->option, hd->host_id_option, MDL);
			
			if (!host_new_hash(&h_id_info->values_hash, HOST_HASH_SIZE, MDL)) 
			{
				log_fatal("No memory for host-identifier option hash.");
			}
			
			h_id_info->relays = hd->relays;
			h_id_info->next = host_id_info;
			host_id_info = h_id_info;
		}

		if (host_hash_lookup(&hp, h_id_info->values_hash, hd->host_id.data, hd->host_id.len, MDL)) 
		{
			/* 
			 * If this option is already present, then add 
			 * this host to the list in n_ipaddr, unless
			 * we have already done so previously.
			 *
			 * XXXSK: This seems scary to me, but I don't
			 *        fully understand how these are used. 
			 *        Shouldn't there be multiple lists, or 
			 *        maybe we should just forbid duplicates?
			 */
			if (np == NULL) 
			{
				np = hp;
				while (np->n_ipaddr != NULL) 
				{
					np = np->n_ipaddr;
				}
				if (hd != np) 
				{
					host_reference(&np->n_ipaddr, hd, MDL);
				}
			}
			host_dereference(&hp, MDL);
		} 
		else 
		{
			host_hash_add(h_id_info->values_hash, 
				      hd->host_id.data,
				      hd->host_id.len,
				      hd, MDL);
		}
	}

	if (dynamicp && commit) 
	{
		if (!write_host(hd))
			return ISC_R_IOERROR;
		if (!commit_leases())
			return ISC_R_IOERROR;
	}

	return ISC_R_SUCCESS;
}

/*********************************************************************
Func Name :   delete_class
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  删除cp指定的class
Input:	      IN struct class * cp
			  IN itn commit
Output:       
Return:       ISC_R_SUCCESS
			  ISC_R_IOERROR
Caution : 	  若没找到返回NULL
*********************************************************************/
isc_result_t delete_class 
(
	struct class * cp, 
	int commit
)
{
	cp->flags |= CLASS_DECL_DELETED;

	/* do the write first as we won't be leaving it in any data
	   structures, unlike the host objects */	
	if (commit) 
	{
		write_named_billing_class((unsigned char *)cp->name, 0, cp);
		if (!commit_leases ())
			return ISC_R_IOERROR;
	}
	
	/*
	 * If this is a subclass remove it from the class's hash table
	 */
	if (cp->superclass) 
	{
		class_hash_delete(cp->superclass->hash, 
				  (const char *)cp->hash_string.data,
				  cp->hash_string.len,
				  MDL);
	}

	/* remove from collections */
	unlink_class(&cp);

	return ISC_R_SUCCESS;
}

/*********************************************************************
Func Name :   delete_host
Date Created: 2018/06/01
Author:  	  wangzhe
Description:  删除主机hd
Input:	      IN struct host_decl * hd
			  IN int commit
Output:       
Return:       ISC_R_SUCCESS
			  ISC_R_IOERROR
Caution : 	  若没找到返回NULL
*********************************************************************/
isc_result_t delete_host 
(
	struct host_decl * hd, 
	int commit
)
{
	struct host_decl *hp = (struct host_decl *)0;
	struct host_decl *np = (struct host_decl *)0;
	struct host_decl *foo;
	int hw_head = 0, uid_head = 1;

	/* 若flag已经置为为delete了，直接返回 */
	/* Don't need to do it twice. */
	if (hd->flags & HOST_DECL_DELETED)
		return ISC_R_SUCCESS;

	/* flags标记置位 */
	/* But we do need to do it once!   :') */
	hd->flags |= HOST_DECL_DELETED;

	/* 若有硬件地址的长度 */
	if (hd->interface.hlen) 
	{
	    if (host_hw_addr_hash) 
		{
			if (host_hash_lookup(&hp, host_hw_addr_hash, hd->interface.hbuf,
				      hd->interface.hlen, MDL)) 
			{
				/* 若找到了，直接删掉 */
		    	if (hp == hd) 
				{
					host_hash_delete(host_hw_addr_hash, hd->interface.hbuf,
				  				hd->interface.hlen, MDL);
					hw_head = 1;
		    	} 
				else 
				{
					np = (struct host_decl *)0;
					foo = (struct host_decl *)0;
					host_reference(&foo, hp, MDL);
					while (foo) 
					{
					    if (foo == hd)
						    break;
					    if (np)
						    host_dereference(&np, MDL);
						
					    host_reference(&np, foo, MDL);
					    host_dereference(&foo, MDL);
						
					    if (np->n_ipaddr)
						    host_reference(&foo, np->n_ipaddr, MDL);
					}

					if (foo) 
					{
					    host_dereference(&np->n_ipaddr, MDL);
					    if (hd->n_ipaddr)
							host_reference(&np->n_ipaddr, hd->n_ipaddr, MDL);
					    host_dereference(&foo, MDL);
					}
					if (np)
						host_dereference(&np, MDL);
		   		 }
		    	host_dereference(&hp, MDL);
			}
	    }
	}

	/* If we got a client identifier, hash this entry by client identifier. */
	if (hd->client_identifier.len) 
	{
	    if (host_uid_hash) 
		{
			if (host_hash_lookup(&hp, host_uid_hash, hd->client_identifier.data,
				      hd->client_identifier.len, MDL)) 
			{
		    	if (hp == hd) 
				{
					host_hash_delete (host_uid_hash, hd->client_identifier.data,
					  		hd->client_identifier.len, MDL);
					uid_head = 1;
		    	} 
				else 
				{
					np = (struct host_decl *)0;
					foo = (struct host_decl *)0;
					host_reference (&foo, hp, MDL);
					while (foo) 
					{
					    if (foo == hd)
						    break;
					    if (np)
							host_dereference(&np, MDL);
						
					    host_reference(&np, foo, MDL);
					    host_dereference(&foo, MDL);
						
					    if (np->n_ipaddr)
						    host_reference(&foo, np->n_ipaddr, MDL);
					}

					if (foo) 
					{
					    host_dereference(&np->n_ipaddr, MDL);
					    if (hd->n_ipaddr)
							host_reference(&np->n_ipaddr, hd->n_ipaddr, MDL);
					    host_dereference(&foo, MDL);
					}
					if (np)
						host_dereference(&np, MDL);
		    	}
		   		host_dereference(&hp, MDL);
			}
	    }
	}

	if (hd->host_id_option != NULL) 
	{
		option_dereference(&hd->host_id_option, MDL);
		data_string_forget(&hd->host_id, MDL);
	}

	if (hd->n_ipaddr) 
	{
		if (uid_head && hd->n_ipaddr->client_identifier.len) 
		{
			host_hash_add(host_uid_hash,
				 hd->n_ipaddr->client_identifier.data,
				 hd->n_ipaddr->client_identifier.len,
				 hd->n_ipaddr, MDL);
		}
		if (hw_head && hd->n_ipaddr->interface.hlen) 
		{
			host_hash_add(host_hw_addr_hash,
				       hd->n_ipaddr->interface.hbuf,
				       hd->n_ipaddr->interface.hlen,
				       hd->n_ipaddr, MDL);
		}
		host_dereference(&hd->n_ipaddr, MDL);
	}

	if (host_name_hash) 
	{
		if (host_hash_lookup(&hp, host_name_hash,
				      (unsigned char *)hd->name,
				      strlen (hd->name), MDL)) 
		{
			if (hp == hd && !(hp -> flags & HOST_DECL_STATIC)) 
			{
				host_hash_delete(host_name_hash, (unsigned char *)hd->name,
						  strlen(hd->name), MDL);
			}
			host_dereference(&hp, MDL);
		}
	}

	if (commit) 
	{
		if (!write_host(hd))
			return ISC_R_IOERROR;
		if (!commit_leases())
			return ISC_R_IOERROR;
	}
	return ISC_R_SUCCESS;
}

/*********************************************************************
Func Name :   find_hosts_by_haddr
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  查找和硬件地址匹配的host，赋值给hp
Input:	      OUT struct host_decl ** hp
			  IN int htype
			  IN const unsigned char * haddr
			  unsigned hlen
			  const char * file
			  int line
Output:       
Return:       int
Caution : 	  
*********************************************************************/
int find_hosts_by_haddr 
(
	struct host_decl **hp, 
	int htype,
	const unsigned char *haddr, 
	unsigned hlen,
	const char *file, 
	int line
)
{
	struct hardware h;
#if defined(LDAP_CONFIGURATION)
	int ret;

	if ((ret = find_haddr_in_ldap(hp, htype, hlen, haddr, file, line)))
		return ret;
#endif

	h.hlen = hlen + 1;
	h.hbuf[0] = htype;
	memcpy(&h.hbuf[1], haddr, hlen);

	return host_hash_lookup(hp, host_hw_addr_hash, h.hbuf, h.hlen, file, line);
}

/*********************************************************************
Func Name :   find_hosts_by_haddr
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  查找和uid匹配的host，赋值给hp
Input:	      OUT struct host_decl ** hp
			  IN unsigned char * data
			  IN unsigned len
			  IN const char * file
			  int line
Output:       
Return:       int
Caution : 	  
*********************************************************************/
int find_hosts_by_uid 
(
	struct host_decl **hp,
    const unsigned char *data, 
    unsigned len,
	const char *file, 
	int line
)
{
	/* hash_lookup */
	return host_hash_lookup(hp, host_uid_hash, data, len, file, line);
}

/*********************************************************************
Func Name :   find_hosts_by_options
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  查找和options匹配的host，赋值给hp
Input:	      OUT struct host_decl ** hp
			  IN struct packet * packet
			  IN struct options_state * opt_state
			  IN const char * file
			  IN int line
Output:       
Return:       int
Caution : 	  
*********************************************************************/
int find_hosts_by_option
(
	struct host_decl **hp, 
    struct packet *packet,
    struct option_state *opt_state,
    const char *file, 
    int line
) 
{
	host_id_info_t *p;
	struct option_cache *oc;
	struct data_string data;
	int found;
	struct packet *relay_packet;
	struct option_state *relay_state;

#if defined(LDAP_CONFIGURATION)
	if ((found = find_client_in_ldap (hp, packet, opt_state, file, line)))
		return found;
#endif
	
	for (p = host_id_info; p != NULL; p = p->next) 
	{
		relay_packet = packet;	
		relay_state = opt_state;

		/* If this option block is for a relay (relays != 0)
		 * and we are processing the main options and not
		 * options from the IA (packet->options == opt_state)
		 * try to find the proper relay
		 */
		if ((p->relays != 0) && (packet->options == opt_state)) 
		{
			int i = p->relays;
			while ((i != 0) && (relay_packet->dhcpv6_container_packet != NULL)) 
			{
				relay_packet = relay_packet->dhcpv6_container_packet;
				i--;
			}
			/* We wanted a specific relay but were
			 * unable to find it */
			if ((p->relays <= MAX_V6RELAY_HOPS) && (i != 0))
				continue;

			relay_state = relay_packet->options;
		}

		oc = lookup_option(p->option->universe, relay_state, p->option->code);
		if (oc != NULL) 
		{
			memset(&data, 0, sizeof(data));

			if (!evaluate_option_cache(&data, relay_packet, NULL,
						   NULL, relay_state, NULL,
						   &global_scope, oc, 
						   MDL)) 
			{
				log_error("Error evaluating option cache");
				return 0;
			}
			
			found = host_hash_lookup(hp, p->values_hash, 
						 data.data, data.len,
						 file, line);

			data_string_forget(&data, MDL);

			if (found) 
			{
				return 1;
			}
		}
	}
	return 0;
}

/*********************************************************************
Func Name :   find_host_for_network
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  找拥有和share一个网段IP地址的host
Input:	      OUT struct subnet ** sp
			  INOUT struct host_decl ** host
			  OUT struct iadr * addr
			  IN struct shared_network * share
Output:       
Return:       void
Caution : 	  More than one host_decl can be returned by find_hosts_by_haddr or
			   find_hosts_by_uid, and each host_decl can have multiple addresses.
			   Loop through the list of hosts, and then for each host, through the
			   list of addresses, looking for an address that's in the same shared
			   network as the one specified.    Store the matching address through
			   the addr pointer, update the host pointer to point at the host_decl
			   that matched, and return the subnet that matched.
*********************************************************************/
int find_host_for_network 
(
	struct subnet **sp, 
	struct host_decl **host,
	struct iaddr *addr, 
	struct shared_network *share
)
{
	int i;
	struct iaddr ip_address;
	struct host_decl *hp;
	struct data_string fixed_addr;

	memset(&fixed_addr, 0, sizeof(fixed_addr));

	/* 遍历host链表 */
	for (hp = *host; hp; hp = hp->n_ipaddr) 
	{
		/* 若没有固定IP地址，继续下一个host */
		if (!hp->fixed_addr)
			continue;

		/* 若从fixed_addr中获取地址失败，继续下一个 */
		if (!evaluate_option_cache(&fixed_addr, (struct packet *)0,
					    (struct lease *)0,
					    (struct client_state *)0,
					    (struct option_state *)0,
					    (struct option_state *)0,
					    &global_scope,
					    hp->fixed_addr, MDL))
			continue;

		/* 若找到地址了，一个host可能有很多，遍历这些地址，找到和share在同一个网段的 */
		for (i = 0; i < fixed_addr.len; i += 4) 
		{
			ip_address.len = 4;
			memcpy(ip_address.iabuf, fixed_addr.data + i, 4);

			/* 若在同一个网段 */
			if (find_grouped_subnet(sp, share, ip_address, MDL)) 
			{
				struct host_decl *tmp = (struct host_decl *)0;
				*addr = ip_address;
				/* This is probably not necessary, but
				   just in case *host is the only reference
				   to that host declaration, make a temporary
				   reference so that dereferencing it doesn't
				   dereference hp out from under us. */
				host_reference(&tmp, *host, MDL);
				host_dereference(host, MDL);
				host_reference(host, hp, MDL);
				host_dereference(&tmp, MDL);
				data_string_forget(&fixed_addr, MDL);
				return 1;
			}
		}
		data_string_forget(&fixed_addr, MDL);
	}
	return 0;
}

/*********************************************************************
Func Name :   new_address_range
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  将一个IP范围段的每一个地址加入hash表
Input:	      IN struct parse * cfile
			  IN struct iaddr low			低地址
			  IN struct iaddr high			高地址
			  struct subnet * subnet
			  struct pool * pool
			  struct lease ** lpchain
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void new_address_range 
(
	struct parse * cfile, 
	struct iaddr low, 
	struct iaddr high, 
	struct subnet * subnet, 
	struct pool * pool, 
	struct lease ** lpchain
)
{
#if defined(COMPACT_LEASES)
	struct lease *address_range;
	unsigned s;
#endif
	unsigned min;		//最小网络的主机号 
	unsigned max; 		//最大网络的主机号
	unsigned i, num_addrs;		
	char lowbuf[16], highbuf[16], netbuf[16];		//转化为字符串的IP地址，只用来打印
	struct shared_network *share = subnet->shared_network;
	struct lease *lt = (struct lease *)0;
#if !defined(COMPACT_LEASES)
	isc_result_t status;
#endif

	/* All subnets should have attached shared network structures. */
	if (!share) 
	{
		/* piaddr将ip地址转化为字符串指针 */
		strcpy(netbuf, piaddr(subnet->net));
		log_fatal("No shared network for network %s (%s)",
		       netbuf, piaddr(subnet->netmask));
	}

	/* 初始化一系列关于lease的hash表，包括uid，ip_addr，hw_addr */
	
	/* Initialize the hash table if it hasn't been done yet. */
	if (!lease_uid_hash) 
	{
		if (!lease_id_new_hash(&lease_uid_hash, LEASE_HASH_SIZE, MDL))
			log_fatal("Can't allocate lease/uid hash");
	}
	if (!lease_ip_addr_hash) 
	{
		if (!lease_ip_new_hash(&lease_ip_addr_hash, LEASE_HASH_SIZE, MDL))
			log_fatal("Can't allocate lease/ip hash");
	}
	if (!lease_hw_addr_hash) 
	{
		if (!lease_id_new_hash(&lease_hw_addr_hash, LEASE_HASH_SIZE, MDL))
			log_fatal("Can't allocate lease/hw hash");
	}

	/* Make sure that high and low addresses are in this subnet. */
	/* 判断一个网址在网段中的方法，与掩码后等于子网地址 */
	if (!addr_eq(subnet->net, subnet_number(low, subnet->netmask))) 
	{
		strcpy(lowbuf, piaddr(low));
		strcpy(netbuf, piaddr(subnet->net));
		log_fatal("bad range, address %s not in subnet %s netmask %s",
			  lowbuf, netbuf, piaddr(subnet->netmask));
	}

	if (!addr_eq(subnet->net, subnet_number(high, subnet->netmask))) 
	{
		strcpy(highbuf, piaddr(high));
		strcpy(netbuf, piaddr(subnet->net));
		log_fatal("bad range, address %s not in subnet %s netmask %s",
			  highbuf, netbuf, piaddr(subnet->netmask));
	}

	/* Get the high and low host addresses... */
	max = host_addr(high, subnet->netmask);
	min = host_addr(low, subnet->netmask);

	/* Allow range to be specified high-to-low as well as low-to-high. */
	/* 这段代码很愚蠢，只是交换min和max而已 */
	if (min > max) 
	{
		max = min;
		min = host_addr(high, subnet->netmask);
	}

	/* get the number of addresses we want, and add it to the pool info
	 * this value is only for use when setting up lease chains and will
	 * be overwritten when expire_all_pools is run
	 */
	/* 获取主机数量 */
	num_addrs = max - min + 1;
#if defined (BINARY_LEASES)
	pool->lease_count += num_addrs;
#endif

	/* Get a lease structure for each address in the range. */
#if defined (COMPACT_LEASES)
	s = (num_addrs + 1) * sizeof (struct lease);
	/* Check unsigned overflow in new_leases().
	   With 304 byte lease structure (x64_86), this happens at
	   range 10.0.0.0 10.215.148.52; */
	/* 检查unsigned溢出 */
	if (((s % sizeof(struct lease)) != 0) ||
	    ((s / sizeof(struct lease)) != (num_addrs + 1))) 
	{
		strcpy(lowbuf, piaddr (low));
		strcpy(highbuf, piaddr (high));
		parse_warn(cfile, "%s-%s is an overly large address range.", lowbuf, highbuf);
		log_fatal("Memory overflow.");
	}
	
	address_range = new_leases(num_addrs, MDL);
	
	if (!address_range) 
	{
		strcpy(lowbuf, piaddr(low));
		strcpy(highbuf, piaddr(high));
		log_fatal ("No memory for address range %s-%s.", lowbuf, highbuf);
	}
#endif

	/* Fill out the lease structures with some minimal information. */
	/* 给每个lease初始化 */
	for (i = 0; i < num_addrs; i++) 
	{
		struct lease *lp = (struct lease *)0;
		/* 下面是给lp分配内存 */
		
#if defined (COMPACT_LEASES)
		/* dhcp_type_lease没有挂接initialize函数 */
		omapi_object_initialize((omapi_object_t *)&address_range[i],
					 dhcp_type_lease, 0, sizeof(struct lease), MDL);
		/* omapi_object_reference */
		lease_reference(&lp, &address_range[i], MDL);
#else
		status = lease_allocate(&lp, MDL);
		if (status != ISC_R_SUCCESS)
			log_fatal("No memory for lease %s: %s",
				   piaddr(ip_addr(subnet->net, subnet->netmask, i + min)),
				   isc_result_totext(status));
#endif

		lp->ip_addr = ip_addr(subnet->net, subnet->netmask, i + min);
		lp->starts 	= MIN_TIME;
		lp->ends 	= MIN_TIME;

		/* 这两个都是简单的内存挂接 */
		subnet_reference(&lp->subnet, subnet, MDL);
		pool_reference(&lp->pool, pool, MDL);
		
		lp->binding_state		 = FTS_FREE;
		lp->next_binding_state   = FTS_FREE;
		lp->rewind_binding_state = FTS_FREE;
		lp->flags 				 = 0;

		/* 将这个地址挂到hash表中 */
		/* Remember the lease in the IP address hash. */
		if (find_lease_by_ip_addr(&lt, lp->ip_addr, MDL)) 
		{
			if (lt->pool) 
			{
				parse_warn(cfile, "lease %s is declared twice!", piaddr(lp->ip_addr));
			} 
			else
			{
				pool_reference(&lt->pool, pool, MDL);
			}
			lease_dereference(&lt, MDL);
		} 
		else
		{
			lease_ip_hash_add(lease_ip_addr_hash,
					  lp->ip_addr.iabuf, lp->ip_addr.len,
					  lp, MDL);
		}
		/* Put the lease on the chain for the caller. */
		if (lpchain) 
		{
			if (*lpchain) 
			{
				lease_reference(&lp->next, *lpchain, MDL);
				lease_dereference(lpchain, MDL);
			}
			lease_reference(lpchain, lp, MDL);
		}
		
		lease_dereference(&lp, MDL);
	}
}

/*********************************************************************
Func Name :   find_sunbet
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  寻找和给定IP地址addr一样的子网号，若找到，挂接到sp指针
Input:	      IN struct subnet **sp
			  IN struct iaddr addr
			  IN const char * file
			  int line
Output:       
Return:       int
Caution : 	  
*********************************************************************/
int find_subnet 
(
	struct subnet **sp,
	struct iaddr addr, 
	const char *file, 
	int line
)
{
	struct subnet *rv;

	for (rv = subnets; rv; rv = rv->next_subnet) 
	{
#if defined(DHCP4o6)
		if (addr.len != rv->netmask.len)
			continue;
#endif
		/* 这个if能走进去 */
		if (addr_eq(subnet_number(addr, rv->netmask), rv->net)) 
		{
			if (subnet_reference(sp, rv, file, line) != ISC_R_SUCCESS)
				return 0;
			return 1;
		}
	}
	return 0;
}

/*********************************************************************
Func Name :   find_sunbet
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  在share_network中寻找和给定IP地址addr一样的子网号
Input:	      IN struct subnet ** sp
			  IN struct shared_network * share
			  IN struct iaddr addr
			  IN const char ** file
			  IN int line
Output:       
Return:       int
Caution : 	  
*********************************************************************/
int find_grouped_subnet 
(
	struct subnet **sp,
    struct shared_network *share, 
    struct iaddr addr,
	const char *file, 
	int line
)
{
	struct subnet *rv;

	for (rv = share->subnets; rv; rv = rv->next_sibling) 
	{
#if defined(DHCP4o6)
		if (addr.len != rv->netmask.len)
			continue;
#endif
		/* 一个网段中的地址与掩码和这个网段是相等的 */
		if (addr_eq(subnet_number(addr, rv->netmask), rv->net)) 
		{
			if(subnet_reference(sp, rv, file, line) != ISC_R_SUCCESS)
				return 0;
			return 1;
		}
	}
	return 0;
}

/*********************************************************************
Func Name :   subnet_inner_than
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  判断subnet是否比scan更inner
Input:	      IN const struct subnet * subnet
			  IN const struct subnet * scan
			  IN int warnp
Output:       
Return:       0
			  1
Caution : 	  could speed up if everyone had a prefix length
*********************************************************************/
int subnet_inner_than
(
	const struct subnet *subnet, 
	const struct subnet *scan,
	int warnp
) 
{
#if defined(DHCP4o6)
	if (subnet->net.len != scan->net.len)
		return 0;
#endif

	/* 保证A和B的前缀一样，网络A与网络B的掩码和B的网络相等，可能B是子网，A是子网内的主机 */
	if (addr_eq(subnet_number(subnet->net, scan->netmask), scan->net) ||
	    addr_eq(subnet_number(scan->net, subnet->netmask), subnet->net)) 
	{
		char n1buf[sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255")];
		int i, j;
		/* 找sunbet最后一个1在的位置，赋值给i */
		for (i = 0; i < 128; i++)
			if (subnet->netmask.iabuf[3 - (i >> 3)] & (1 << (i & 7)))
				break;
		for (j = 0; j < 128; j++)
			if (scan->netmask.iabuf[3 - (j >> 3)] & (1 << (j & 7)))
				break;
		if (warnp) 
		{
			strcpy(n1buf, piaddr(subnet->net));
			log_error("Warning: subnet %s/%d overlaps subnet %s/%d",
			      n1buf, 32 - i,
			      piaddr(scan->net), 32 - j);
		}
		if (i < j)
			return 1;
	}
	return 0;
}

/*********************************************************************
Func Name :   enter_subnet
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  将subnet插入到全局subnets链表中
Input:	      IN struct sunbet * subnet
Output:       
Return:       
Caution : 	  Enter a new subnet into the subnet list
*********************************************************************/
void enter_subnet 
(
	struct subnet * subnet
)
{
	struct subnet *scan = (struct subnet *)0;
	struct subnet *next = (struct subnet *)0;
	struct subnet *prev = (struct subnet *)0;

	/* Check for duplicates... */
	if (subnets)
	{
		subnet_reference(&next, subnets, MDL);
	}
	
	while (next) 
	{
	    subnet_reference(&scan, next, MDL);
	    subnet_dereference(&next, MDL);

	    /* When we find a conflict, make sure that the
	       subnet with the narrowest subnet mask comes
	       first. */
	    if (subnet_inner_than(subnet, scan, 1)) 
		{
			if (prev) 
			{
			    if (prev->next_subnet)
				{
					subnet_dereference(&prev->next_subnet, MDL);
			    }
			    subnet_reference(&prev->next_subnet, subnet, MDL);
			    subnet_dereference(&prev, MDL);
			} 
			else 
			{
			    subnet_dereference(&subnets, MDL);
			    subnet_reference(&subnets, subnet, MDL);
			}
			subnet_reference(&subnet->next_subnet, scan, MDL);
			subnet_dereference(&scan, MDL);
			
			return;
	    }
	    subnet_reference(&prev, scan, MDL);
	    subnet_dereference(&scan, MDL);
	}
	
	if (prev)
	{
		subnet_dereference(&prev, MDL);
	}

	/* XXX use the BSD radix tree code instead of a linked list. */
	if (subnets) 
	{
		/* 将subnet插入subnets链表头 */
		subnet_reference(&subnet->next_subnet, subnets, MDL);
		subnet_dereference(&subnets, MDL);
	}
	subnet_reference(&subnets, subnet, MDL);
}

/*********************************************************************
Func Name :   enter_shared_network
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  Enter a new shared network into the shared network list
Input:	      IN struct shared_network * share
Output:       
Return:       
Caution : 	  将share插入shared_networks的链表头
*********************************************************************/
void enter_shared_network 
(
	struct shared_network * share
)
{
	if (shared_networks) 
	{
		shared_network_reference(&share->next, shared_networks, MDL);
		shared_network_dereference(&shared_networks, MDL);
	}
	shared_network_reference(&shared_networks, share, MDL);
}

/*********************************************************************
Func Name :   new_shared_network_interface
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  
Input:	      IN struct shared_network * share
Output:       
Return:       
Caution : 	  
*********************************************************************/	
void new_shared_network_interface 
(
	struct parse * cfile, 
	struct shared_network * share, 
	const char * name
)
{
	struct interface_info *ip;
	isc_result_t status;

	if (share->interface) 
	{
		parse_warn(cfile, 
			    "A subnet or shared network can't be connected %s",
			    "to two interfaces.");
		return;
	}

	/* 在interface链表中查找和name匹配的网络接口 */
	for (ip = interfaces; ip; ip = ip->next)
		if (!strcmp(ip->name, name))
			break;
		
	if (!ip) 
	{
		/* 若没找到，给ip分配内存 */
		status = interface_allocate(&ip, MDL);
		if (status != ISC_R_SUCCESS)
			log_fatal("new_shared_network_interface %s: %s",
				   name, isc_result_totext(status));

		/* 若name长度超长，手动截断 */
		if (strlen(name) > sizeof(ip->name)) 
		{
			memcpy(ip->name, name, (sizeof(ip->name)) - 1);
			ip->name[(sizeof(ip->name)) - 1] = 0;
		} 
		else
		{
			strcpy(ip->name, name);
		}

		/* 将ip插入interfaces链表头 */
		if (interfaces) 
		{
			interface_reference(&ip->next, interfaces, MDL);
			interface_dereference(&interfaces, MDL);
		}	
		interface_reference(&interfaces, ip, MDL);
		ip->flags = INTERFACE_REQUESTED;
		
		/* XXX this is a reference loop. */
		shared_network_reference(&ip->shared_network, share, MDL);
		interface_reference(&share->interface, ip, MDL);
	}
}

/*********************************************************************
Func Name :   enter_lease
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将lease插入或更新到ip的hash表中
Input:	      INOUT struct lease * lease
Output:       
Return:       void
Caution : 	  Enter a lease into the system.   This is called by the parser each
 			   time it reads in a new lease.   If the subnet for that lease has
 			   already been read in (usually the case), just update that lease;
 			   otherwise, allocate temporary storage for the lease and keep it around
 			   until we're done reading in the config file
*********************************************************************/
void enter_lease 
(
	struct lease * lease
)
{
	struct lease *comp = (struct lease *)0;

	/* 使用lease给定的ip_addr从ip的hash表中寻找lease，若找到，将pool和subnet赋值给lease */
	if (find_lease_by_ip_addr(&comp, lease->ip_addr, MDL)) 
	{
		if (!comp->pool) 
		{
			log_error("undeclared lease found in database: %s",
				   piaddr(lease->ip_addr));
		} 
		else
		{
			pool_reference(&lease->pool, comp->pool, MDL);
		}

		if (comp->subnet)
		{
			subnet_reference(&lease->subnet, comp->subnet, MDL);
		}
		lease_ip_hash_delete(lease_ip_addr_hash, lease->ip_addr.iabuf, 
						lease->ip_addr.len, MDL);
		lease_dereference(&comp, MDL);
	}

	/* The only way a lease can get here without a subnet is if it's in
	   the lease file, but not in the dhcpd.conf file.  In this case, we
	   *should* keep it around until it's expired, but never reallocate it
	   or renew it.  Currently, to maintain consistency, we are not doing
	   this.
	   XXX fix this so that the lease is kept around until it expires.
	   XXX this will be important in IPv6 with addresses that become
	   XXX non-renewable as a result of a renumbering event. */

	if (!lease->subnet) 
	{
		log_error("lease %s: no subnet.", piaddr(lease->ip_addr));
		return;
	}
	lease_ip_hash_add(lease_ip_addr_hash, lease->ip_addr.iabuf,
					lease->ip_addr.len, lease, MDL);
}

/*********************************************************************
Func Name :   supersede_lease
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  用lease替换comp，更新hash表和排序
Input:	      
Output:       
Return:       
Caution : 	  Replace the data in an existing lease with the data in a new lease;
			   adjust hash tables to suit, and insertion sort the lease into the
			   list of leases by expiry time so that we can always find the oldest
			   lease
*********************************************************************/
int supersede_lease 
(
	struct lease * comp, 
	struct lease * lease, 
	int commit, 
	int propogate, 
	int pimmediate, 
	int from_pool
)
{
	LEASE_STRUCT_PTR lq;
	struct timeval tv;
#if defined (FAILOVER_PROTOCOL)
	int do_pool_check = 0;

	/* We must commit leases before sending updates regarding them
	   to failover peers.  It is, therefore, an error to set pimmediate
	   and not commit. */
	if (pimmediate && !commit)
		return 0;
#endif
	/* If there is no sample lease, just do the move. */
	if (!lease)
		goto just_move_it;

	/* Static leases are not currently kept in the database... */
	if (lease->flags & STATIC_LEASE)
		return 1;

	/* 两个租约冲突的情况，比如具有不同的硬件地址 */
	/* If the existing lease hasn't expired and has a different
	   unique identifier or, if it doesn't have a unique
	   identifier, a different hardware address, then the two
	   leases are in conflict.  If the existing lease has a uid
	   and the new one doesn't, but they both have the same
	   hardware address, and dynamic bootp is allowed on this
	   lease, then we allow that, in case a dynamic BOOTP lease is
	   requested *after* a DHCP lease has been assigned. */
	if (lease->binding_state != FTS_ABANDONED && 
		lease->next_binding_state != FTS_ABANDONED &&
	    comp->binding_state == FTS_ACTIVE &&
	    (
	    /* comp和lease的uid不一样 */
	    ((comp->uid && lease->uid) &&
	      (comp->uid_len != lease->uid_len ||
	       memcmp(comp->uid, lease->uid, comp->uid_len))) ||
	    /* comp没有uid并且comp和lease的硬件地址不一致 */
	     (!comp->uid &&
	      ((comp->hardware_addr.hlen != lease->hardware_addr.hlen) ||
	       memcmp(comp->hardware_addr.hbuf,
		       lease->hardware_addr.hbuf,
		       comp->hardware_addr.hlen))))) 
	{
		log_error ("Lease conflict at %s", piaddr (comp -> ip_addr));
	}

	/* If there's a Unique ID, dissociate it from the hash
	   table and free it if necessary. */
	if (comp->uid) 
	{
		uid_hash_delete(comp);
		if (comp->uid != comp->uid_buf) 
		{
			dfree(comp->uid, MDL);
			comp->uid_max = 0;
			comp->uid_len = 0;
		}
		comp->uid = (unsigned char *)0;
	}

	/* If there's a hardware address, remove the lease from its
	 * old position in the hash bucket's ordered list.
	 */
	if (comp->hardware_addr.hlen)
		hw_hash_delete(comp);

	/* If the lease has been billed to a class, remove the billing. */
	if (comp->billing_class != lease->billing_class) 
	{
		if (comp->billing_class)
			unbill_class(comp);
		if (lease->billing_class)
			bill_class(comp, lease->billing_class);
	}

	/* Copy the data files, but not the linkages. */

	comp->starts = lease->starts;

	/* 复制uid */
	if (lease->uid) 
	{
		if (lease->uid_len <= sizeof(lease->uid_buf)) 
		{
			memcpy(comp->uid_buf, lease->uid, lease->uid_len);
			comp->uid 	  = &comp->uid_buf[0];
			comp->uid_max = sizeof(comp->uid_buf);
			comp->uid_len = lease->uid_len;
		} 
		else if (lease->uid != &lease->uid_buf[0]) 
		{
			comp->uid	   = lease->uid;
			comp->uid_max  = lease->uid_max;
			lease->uid     = (unsigned char *)0;
			lease->uid_max = 0;
			comp->uid_len  = lease->uid_len;
			lease->uid_len = 0;
		} 
		else 
		{
			log_fatal ("corrupt lease uid."); /* XXX */
		}
	} 
	else 
	{
		comp->uid = (unsigned char *)0;
		comp->uid_len = comp->uid_max = 0;
	}

	/* 复制host */
	if (comp->host)
	{
		host_dereference(&comp->host, MDL);
	}
	host_reference(&comp->host, lease->host, MDL);

	/* 复制硬件地址 */
	comp->hardware_addr = lease->hardware_addr;

	/* 复制scope */
	if (comp->scope)
	{
		binding_scope_dereference(&comp->scope, MDL);
	}
	
	if (lease->scope) 
	{
		binding_scope_reference(&comp->scope, lease->scope, MDL);
		binding_scope_dereference(&lease->scope, MDL);
	}

	/* 复制agent_options */
	if (comp->agent_options)
	{
		option_chain_head_dereference(&comp->agent_options, MDL);
	}
	
	if (lease->agent_options) 
	{
		/* Only retain the agent options if the lease is still
		   affirmatively associated with a client. */
		/* 仅保留agent信息当lease和client关联的时候 */
		if (lease->next_binding_state == FTS_ACTIVE ||
		    lease->next_binding_state == FTS_EXPIRED)
		{
			option_chain_head_reference(&comp->agent_options,
						     lease->agent_options, MDL);
		}
		option_chain_head_dereference(&lease->agent_options, MDL);
	}

	/* 复制主机名 */
	if (comp->client_hostname)
	{
		dfree(comp->client_hostname, MDL);
	}
	
	comp->client_hostname = lease->client_hostname;
	lease->client_hostname = (char *)0;

	/* 复制on_star下的3个字段 */
	if (lease->on_star.on_expiry) 
	{
		if (comp->on_star.on_expiry)
			executable_statement_dereference(&comp->on_star.on_expiry, MDL);
		executable_statement_reference(&comp->on_star.on_expiry,
						lease->on_star.on_expiry, MDL);
	}
	if (lease->on_star.on_commit) 
	{
		if (comp->on_star.on_commit)
			executable_statement_dereference(&comp->on_star.on_commit, MDL);
		executable_statement_reference (&comp->on_star.on_commit,
						lease->on_star.on_commit, MDL);
	}
	if (lease->on_star.on_release) 
	{
		if (comp->on_star.on_release)
			executable_statement_dereference(&comp->on_star.on_release, MDL);
		executable_statement_reference (&comp->on_star.on_release,
						lease->on_star.on_release,MDL);
	}

	/* Record the lease in the uid hash if necessary. */
	if (comp->uid)
	{
		uid_hash_add(comp);
	}

	/* Record it in the hardware address hash if necessary. */
	if (comp->hardware_addr.hlen)
	{
		hw_hash_add(comp);
	}

	/* 复制其他字段 */
	comp->cltt = lease->cltt;
#if defined (FAILOVER_PROTOCOL)
	comp->tstp  = lease->tstp;
	comp->tsfp  = lease->tsfp;
	comp->atsfp = lease->atsfp;
#endif /* FAILOVER_PROTOCOL */
	comp->ends = lease->ends;
	comp->next_binding_state = lease->next_binding_state;

	/*
	 * If we have a control block pointer copy it in.
	 * We don't zero out an older ponter as it is still
	 * in use.  We shouldn't need to overwrite an
	 * old pointer with a new one as the old transaction
	 * should have been cancelled before getting here.
	 */
	if (lease->ddns_cb != NULL)
		comp->ddns_cb = lease->ddns_cb;

      just_move_it:
#if defined (FAILOVER_PROTOCOL)
	/*
	 * Atsfp should be cleared upon any state change that implies
	 * propagation whether supersede_lease was given a copy lease
	 * structure or not (often from the pool_timer()).
	 */
	if (propogate)
		comp->atsfp = 0;
#endif /* FAILOVER_PROTOCOL */

	if (!comp->pool) 
	{
		log_error("Supersede_lease: lease %s with no pool.", piaddr(comp->ip_addr));
		return 0;
	}

	/* Figure out which queue it's on. */
	switch (comp->binding_state) 
	{
	      case FTS_FREE:
		if (comp->flags & RESERVED_LEASE)
		{
			lq = &comp->pool->reserved;
		}
		else 
		{
			lq = &comp->pool->free;
			comp->pool->free_leases--;
		}

#if defined(FAILOVER_PROTOCOL)
		do_pool_check = 1;
#endif
		break;

	      case FTS_ACTIVE:
		lq = &comp->pool->active;
		break;

	      case FTS_EXPIRED:
	      case FTS_RELEASED:
	      case FTS_RESET:
		lq = &comp->pool->expired;
		break;

	      case FTS_ABANDONED:
		lq = &comp->pool->abandoned;
		break;

	      case FTS_BACKUP:
		if (comp->flags & RESERVED_LEASE)
			lq = &comp->pool->reserved;
		else {
			lq = &comp->pool->backup;
			comp->pool->backup_leases--;
		}

#if defined(FAILOVER_PROTOCOL)
		do_pool_check = 1;
#endif
		break;

	      default:
		log_error ("Lease with bogus binding state: %d", comp->binding_state);
#if defined (BINDING_STATE_DEBUG)
		abort ();
#endif
		return 0;
	}

	/* Remove the lease from its current place in its current
	   timer sequence. */
	LEASE_REMOVEP(lq, comp);

	/* Now that we've done the flag-affected queue removal
	 * we can update the new lease's flags, if there's an
	 * existing lease */
	if (lease) 
	{
		comp->flags = ((lease->flags & ~PERSISTENT_FLAGS) | (comp->flags & ~EPHEMERAL_FLAGS));
	}

	/* Make the state transition. */
	if (commit || !pimmediate)
	{
		make_binding_state_transition(comp);
	}

	/* Put the lease back on the appropriate queue.    If the lease
	   is corrupt (as detected by lease_enqueue), don't go any farther. */
	if (!lease_enqueue(comp))
		return 0;

	/* If this is the next lease that will timeout on the pool,
	   zap the old timeout and set the timeout on this pool to the
	   time that the lease's next event will happen.
		   
	   We do not actually set the timeout unless commit is true -
	   we don't want to thrash the timer queue when reading the
	   lease database.  Instead, the database code calls the
	   expiry event on each pool after reading in the lease file,
	   and the expiry code sets the timer if there's anything left
	   to expire after it's run any outstanding expiry events on
	   the pool. */
	if ((commit || !pimmediate) &&
	    comp->sort_time != MIN_TIME &&
	    comp->sort_time > cur_time &&
	    (comp->sort_time < comp->pool->next_event_time ||
	     comp->pool->next_event_time == MIN_TIME)) 
	{
		/* 设置下一个事件发生时间为租约到期时间 */
		comp->pool->next_event_time = comp->sort_time;
		tv.tv_sec = comp->pool->next_event_time;
		tv.tv_usec = 0;
		add_timeout(&tv,
			     pool_timer, comp->pool,
			     (tvref_t)pool_reference,
			     (tvunref_t)pool_dereference);
	}

	if (commit) 
	{
#if defined(FAILOVER_PROTOCOL)
		/*
		 * If commit and propogate are set, then we can save a
		 * possible fsync later in BNDUPD socket transmission by
		 * stepping the rewind state forward to the new state, in
		 * case it has changed.  This is only worth doing if the
		 * failover connection is currently connected, as in this
		 * case it is likely we will be transmitting to the peer very
		 * shortly.
		 */
		if (propogate && (comp->pool->failover_peer != NULL) &&
		    ((comp->pool->failover_peer->service_state ==
							    cooperating) ||
		     (comp->pool->failover_peer->service_state ==
							    not_responding)))
			comp->rewind_binding_state = comp->binding_state;
#endif

		if (!write_lease(comp))
			return 0;
		if ((server_starting & SS_NOSYNC) == 0) {
			if (!commit_leases())
				return 0;
		}
	}

#if defined (FAILOVER_PROTOCOL)
	if (propogate) 
	{
		comp->desired_binding_state = comp->binding_state;
		if (!dhcp_failover_queue_update (comp, pimmediate))
			return 0;
	}
	if (do_pool_check && comp->pool->failover_peer)
		dhcp_failover_pool_check(comp->pool);
#endif

	/* If the current binding state has already expired and we haven't
	 * been called from pool_timer, do an expiry event right now.
	 */
	/* XXX At some point we should optimize this so that we don't
	   XXX write the lease twice, but this is a safe way to fix the
	   XXX problem for 3.0 (I hope!). */
	if ((from_pool == 0) &&
	    (commit || !pimmediate) &&
	    (comp->sort_time < cur_time) &&
	    (comp->next_binding_state != comp->binding_state))
		pool_timer(comp->pool);

	return 1;
}

/*********************************************************************
Func Name :   make_bingding_state_transition
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  进行状态转换
Input:	      IN struct lease * lease
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void make_binding_state_transition 
(
	struct lease *lease
)
{

#if defined (FAILOVER_PROTOCOL)
	dhcp_failover_state_t *peer;

	if (lease -> pool && lease -> pool -> failover_peer)
		peer = lease -> pool -> failover_peer;
	else
		peer = (dhcp_failover_state_t *)0;
#endif

	/* If the lease was active and is now no longer active, but isn't
	   released, then it just expired, so do the expiry event. */
	if (lease->next_binding_state != lease->binding_state &&
	    ((
#if defined (FAILOVER_PROTOCOL)
		    peer &&
		    (lease->binding_state == FTS_EXPIRED ||
		     lease->binding_state == FTS_ACTIVE) &&
		    (lease->next_binding_state == FTS_FREE ||
		     lease->next_binding_state == FTS_BACKUP)) ||
	     (!peer &&
#endif
	      lease->binding_state == FTS_ACTIVE &&
	      lease->next_binding_state != FTS_RELEASED))) 
	{
#if defined (NSUPDATE)
		(void)ddns_removals(lease, NULL, NULL, ISC_TRUE);
#endif
		if (lease->on_star.on_expiry) 
		{
			execute_statements(NULL, NULL, lease,
					   NULL, NULL, NULL,
					   &lease->scope,
					   lease->on_star.on_expiry,
					   NULL);
			if (lease->on_star.on_expiry)
			{
				executable_statement_dereference(&lease->on_star.on_expiry, MDL);
			}
		}
		
		/* No sense releasing a lease after it's expired. */
		if (lease->on_star.on_release)
		{
			executable_statement_dereference(&lease->on_star.on_release, MDL);
		}
		/* Get rid of client-specific bindings that are only
		   correct when the lease is active. */
		if (lease->billing_class)
			unbill_class(lease);
		if (lease->agent_options)
			option_chain_head_dereference (&lease -> agent_options,
						       MDL);
		if (lease->client_hostname) 
		{
			dfree(lease->client_hostname, MDL);
			lease->client_hostname = (char *)0;
		}
		if (lease->host)
			host_dereference(&lease->host, MDL);

		/* Send the expiry time to the peer. */
		lease->tstp = lease->ends;
	}

	/* If the lease was active and is now released, do the release event. */
	if (lease->next_binding_state != lease->binding_state &&
	    ((
#if defined (FAILOVER_PROTOCOL)
		    peer &&
		    lease->binding_state == FTS_RELEASED &&
		    (lease->next_binding_state == FTS_FREE ||
		     lease->next_binding_state == FTS_BACKUP)) ||
	     (!peer &&
#endif
	      lease->binding_state == FTS_ACTIVE &&
	      lease->next_binding_state == FTS_RELEASED))) 
	{
#if defined (NSUPDATE)
		/*
		 * Note: ddns_removals() is also iterated when the lease
		 * enters state 'released' in 'release_lease()'.  The below
		 * is caught when a peer receives a BNDUPD from a failover
		 * peer; it may not have received the client's release (it
		 * may have been offline).
		 *
		 * We could remove the call from release_lease() because
		 * it will also catch here on the originating server after the
		 * peer acknowledges the state change.  However, there could
		 * be many hours inbetween, and in this case we /know/ the
		 * client is no longer using the lease when we receive the
		 * release message.  This is not true of expiry, where the
		 * peer may have extended the lease.
		 */
		(void)ddns_removals(lease, NULL, NULL, ISC_TRUE);
#endif
		if (lease->on_star.on_release) 
		{
			execute_statements(NULL, NULL, lease,
					   NULL, NULL, NULL,
					   &lease->scope,
					   lease->on_star.on_release,
					   NULL);
			executable_statement_dereference(&lease->on_star.on_release, MDL);
		}
		
		/* A released lease can't expire. */
		if (lease->on_star.on_expiry)
			executable_statement_dereference(&lease->on_star.on_expiry, MDL);

		/* Get rid of client-specific bindings that are only
		   correct when the lease is active. */
		if (lease->billing_class)
			unbill_class(lease);
		if (lease->agent_options)
			option_chain_head_dereference(&lease->agent_options,
						       MDL);
		if (lease->client_hostname) {
			dfree(lease->client_hostname, MDL);
			lease->client_hostname = (char *)0;
		}
		if (lease->host)
			host_dereference(&lease->host, MDL);

		/* Send the release time (should be == cur_time) to the
		   peer. */
		lease->tstp = lease->ends;
	}

#if defined (DEBUG_LEASE_STATE_TRANSITIONS)
	log_debug ("lease %s moves from %s to %s",
		   piaddr (lease -> ip_addr),
		   binding_state_print (lease -> binding_state),
		   binding_state_print (lease -> next_binding_state));
#endif

	lease->binding_state = lease->next_binding_state;
	switch (lease->binding_state) 
	{
	      case FTS_ACTIVE:
#if defined (FAILOVER_PROTOCOL)
		if (lease -> pool && lease -> pool -> failover_peer)
			lease -> next_binding_state = FTS_EXPIRED;
		else
#endif
			lease -> next_binding_state = FTS_FREE;
		break;

	      case FTS_EXPIRED:
	      case FTS_RELEASED:
	      case FTS_ABANDONED:
	      case FTS_RESET:
		lease->next_binding_state = FTS_FREE;
#if defined(FAILOVER_PROTOCOL)
		/* If we are not in partner_down, leases don't go from
		   EXPIRED to FREE on a timeout - only on an update.
		   If we're in partner_down, they expire at mclt past
		   the time we entered partner_down. */
		if ((lease->pool != NULL) &&
		    (lease->pool->failover_peer != NULL) &&
		    (lease->pool->failover_peer->me.state == partner_down))
			lease->tsfp =
			    (lease->pool->failover_peer->me.stos +
			     lease->pool->failover_peer->mclt);
#endif /* FAILOVER_PROTOCOL */
		break;

	      case FTS_FREE:
	      case FTS_BACKUP:
		lease->next_binding_state = lease->binding_state;
		break;
	}
#if defined (DEBUG_LEASE_STATE_TRANSITIONS)
	log_debug ("lease %s: next binding state %s",
		   piaddr (lease -> ip_addr),
		   binding_state_print (lease -> next_binding_state));
#endif
}

/*********************************************************************
Func Name :   lease_copy
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将lease复制到lp
Input:	      IN struct lease ** lp
			  IN strcut lease * lease
			  IN const char * file
			  IN int line
Output:       
Return:       void
Caution : 	  Copy the contents of one lease into another, correctly maintaining
   				reference counts
*********************************************************************/
int lease_copy 
(
	struct lease **lp,
	struct lease *lease, 
	const char *file, 
	int line
)
{
	struct lease *lt = (struct lease *)0;
	isc_result_t status;

	/* 给lt申请内存并复制相关字段 */
	status = lease_allocate(&lt, MDL);
	if (status != ISC_R_SUCCESS)
		return 0;

	lt->ip_addr = lease->ip_addr;
	lt->starts  = lease->starts;
	lt->ends    = lease->ends;
	lt->uid_len = lease->uid_len;
	lt->uid_max = lease->uid_max;

	/* 复制uid */
	if (lease->uid == lease->uid_buf) 
	{
		lt->uid = lt->uid_buf;
		memcpy(lt->uid_buf, lease->uid_buf, sizeof(lt->uid_buf));
	} 
	else if (!lease->uid_max) 
	{
		lt->uid = (unsigned char *)0;
	} 
	else 
	{
		lt->uid = dmalloc(lt->uid_max, MDL);
		if (!lt->uid) 
		{
			lease_dereference (&lt, MDL);
			return 0;
		}
		memcpy(lt->uid, lease->uid, lease->uid_max);
	}

	/* 复制hostname */
	if (lease->client_hostname) 
	{
		lt->client_hostname =
			dmalloc(strlen(lease->client_hostname) + 1, MDL);
		if (!lt->client_hostname) 
		{
			lease_dereference(&lt, MDL);
			return 0;
		}
		strcpy(lt->client_hostname, lease->client_hostname);
	}

	/* 复制scope和agent_options */
	if (lease->scope)
		binding_scope_reference(&lt->scope, lease->scope, MDL);
	if (lease->agent_options)
		option_chain_head_reference(&lt->agent_options,
					     lease->agent_options, MDL);

	/* 复制若干指针 */
	host_reference(&lt->host, lease->host, file, line);
	subnet_reference(&lt->subnet, lease->subnet, file, line);
	pool_reference(&lt->pool, lease->pool, file, line);
	class_reference(&lt->billing_class, lease->billing_class, file, line);
	lt->hardware_addr = lease->hardware_addr;

	/* 复制on_star下的3个字段 */
	if (lease->on_star.on_expiry)
	{
		executable_statement_reference(&lt->on_star.on_expiry,
						lease->on_star.on_expiry,
						file, line);
	}
	
	if (lease->on_star.on_commit)
	{
		executable_statement_reference(&lt->on_star.on_commit,
						lease->on_star.on_commit,
						file, line);
	}
	
	if (lease->on_star.on_release)
	{
		executable_statement_reference(&lt->on_star.on_release,
						lease->on_star.on_release,
						file, line);
	}

	/* 其他字段赋值 */
	lt->flags 		  		 = lease->flags;
	lt->tstp  		 		 = lease->tstp;
	lt->tsfp  		 		 = lease->tsfp;
	lt->atsfp 		 		 = lease->atsfp;
	lt->cltt  		 		 = lease->cltt;
	lt->binding_state 	   	 = lease->binding_state;
	lt->next_binding_state 	 = lease->next_binding_state;
	lt->rewind_binding_state = lease->rewind_binding_state;

	/* 将lt挂接到lp，并释放lt临时结构 */
	status = lease_reference(lp, lt, file, line);
	lease_dereference(&lt, MDL);
	
	return status == ISC_R_SUCCESS;
}

/*********************************************************************
Func Name :   release_lease
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  释放特定租约并处理hash表，这里只处理通过池动态分配的租约
Input:	      IN struct lease * lease
			  IN struct packet * packet
Output:       
Return:       void
Caution : 	  Release the specified lease and re-hash it as appropriate
*********************************************************************/
void release_lease 
(
	struct lease * lease, 
	struct packet * packet
)
{
	/* If there are statements to execute when the lease is
	   released, execute them. */
#if defined (NSUPDATE)
	(void) ddns_removals(lease, NULL, NULL, ISC_FALSE);
#endif
	if (lease->on_star.on_release) 
	{
		execute_statements(NULL, packet, lease,
				    NULL, packet->options,
				    NULL, &lease->scope,
				    lease->on_star.on_release, NULL);
		if (lease->on_star.on_release)
			executable_statement_dereference
				(&lease->on_star.on_release, MDL);
	}

	/* We do either the on_release or the on_expiry events, but
	   not both (it's possible that they could be the same,
	   in any case). */
	if (lease->on_star.on_expiry)
		executable_statement_dereference(&lease->on_star.on_expiry, MDL);

	if (lease->binding_state != FTS_FREE &&
	    lease->binding_state != FTS_BACKUP &&
	    lease->binding_state != FTS_RELEASED &&
	    lease->binding_state != FTS_EXPIRED &&
	    lease->binding_state != FTS_RESET) 
	{
		if (lease->on_star.on_commit)
			executable_statement_dereference(&lease->on_star.on_commit, MDL);

		/* Blow away any bindings. */
		if (lease->scope)
			binding_scope_dereference(&lease->scope, MDL);

		/* Set sort times to the present. */
		lease->ends = cur_time;
		/* Lower layers of muckery set tstp to ->ends.  But we send
		 * protocol messages before this.  So it is best to set
		 * tstp now anyway.
		 */
		lease->tstp = cur_time;
#if defined (FAILOVER_PROTOCOL)
		if (lease -> pool && lease -> pool -> failover_peer) {
			dhcp_failover_state_t *peer = NULL;

			if (lease->pool != NULL)
				peer = lease->pool->failover_peer;

			if ((peer->service_state == not_cooperating) &&
			    (((peer->i_am == primary) &&
			      (lease->rewind_binding_state == FTS_FREE)) ||
			     ((peer->i_am == secondary) &&
			      (lease->rewind_binding_state == FTS_BACKUP)))) {
				lease->next_binding_state =
						  lease->rewind_binding_state;
			} else
				lease -> next_binding_state = FTS_RELEASED;
		} else {
			lease -> next_binding_state = FTS_FREE;
		}
#else
		lease->next_binding_state = FTS_FREE;
#endif
		supersede_lease(lease, NULL, 1, 1, 1, 0);
	}
}

/*********************************************************************
Func Name :   abandon_lease
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  放弃指定的租约
Input:	      IN struct lease * lease
			  IN const char * message
Output:       
Return:       void
Caution : 	  Abandon the specified lease (set its timeout to infinity and its
  				 particulars to zero, and re-hash it as appropriate.
*********************************************************************/
void abandon_lease 
(
	struct lease * lease, 
	const char * message
)
{
	struct lease *lt = NULL;
#if defined (NSUPDATE)
	(void)ddns_removals(lease, NULL, NULL, ISC_FALSE);
#endif

	/* 将lease复制到临时结构lt */
	if (!lease_copy(&lt, lease, MDL)) 
	{
		return;
	}

	/* 释放scope */
	if (lt->scope) 
	{
		binding_scope_dereference(&lt->scope, MDL);
	}

	/* Calculate the abandone expiry time.  If it wraps,
 	 * use the maximum expiry time. */
	lt->ends = cur_time + abandon_lease_time;
	if (lt->ends < cur_time || lt->ends > MAX_TIME) 
	{
		lt->ends = MAX_TIME;
	}

	lt->next_binding_state = FTS_ABANDONED;

	log_error("Abandoning IP address %s: %s", piaddr(lease->ip_addr), message);
	
	/* 硬件长度清零 */
	lt->hardware_addr.hlen = 0;

	/* 释放uid */
	if (lt->uid && lt->uid != lt->uid_buf) 
	{
		dfree(lt->uid, MDL);
	}

	lt->uid 	= NULL;
	lt->uid_len = 0;
	lt->uid_max = 0;

	/* 用lt替换lease并释放lt临时结构 */
	supersede_lease(lease, lt, 1, 1, 1, 0);
	lease_dereference(&lt, MDL);
}

#if 0
/*
 * This doesn't appear to be in use for anything anymore.
 * I'm ifdeffing it now and if there are no complaints in
 * the future it will be removed.
 * SAR
 */

/* Abandon the specified lease (set its timeout to infinity and its
   particulars to zero, and re-hash it as appropriate. */

void dissociate_lease (lease)
	struct lease *lease;
{
	struct lease *lt = (struct lease *)0;
#if defined (NSUPDATE)
	(void) ddns_removals(lease, NULL, NULL, ISC_FALSE);
#endif

	if (!lease_copy (&lt, lease, MDL))
		return;

#if defined (FAILOVER_PROTOCOL)
	if (lease -> pool && lease -> pool -> failover_peer) {
		lt -> next_binding_state = FTS_RESET;
	} else {
		lt -> next_binding_state = FTS_FREE;
	}
#else
	lt -> next_binding_state = FTS_FREE;
#endif
	lt -> ends = cur_time; /* XXX */
	lt -> hardware_addr.hlen = 0;
	if (lt -> uid && lt -> uid != lt -> uid_buf)
		dfree (lt -> uid, MDL);
	lt -> uid = (unsigned char *)0;
	lt -> uid_len = 0;
	lt -> uid_max = 0;
	supersede_lease (lease, lt, 1, 1, 1, 0);
	lease_dereference (&lt, MDL);
}
#endif

/*********************************************************************
Func Name :   pool_timer
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  当特定池中的租约到期，调用此函数，处理所有过期的租约，也可以手动调用
Input:	      void * vpool
Output:       
Return:       void
Caution : 	  Timer called when a lease in a particular pool expires
*********************************************************************/
void pool_timer 
(
	void * vpool
)
{
	struct pool *pool;
	struct lease *next = NULL;
	struct lease *lease = NULL;
	struct lease *ltemp = NULL;
#define FREE_LEASES 0
#define ACTIVE_LEASES 1
#define EXPIRED_LEASES 2
#define ABANDONED_LEASES 3
#define BACKUP_LEASES 4
#define RESERVED_LEASES 5
	LEASE_STRUCT_PTR lptr[RESERVED_LEASES + 1];
	TIME next_expiry = MAX_TIME;			//下一个到期的租约
	int i;
	struct timeval tv;

	pool = (struct pool *)vpool;

	/* 接管pool下的几种链表 */
	lptr[FREE_LEASES] 	   = &pool->free;
	lptr[ACTIVE_LEASES]    = &pool->active;
	lptr[EXPIRED_LEASES]   = &pool->expired;
	lptr[ABANDONED_LEASES] = &pool->abandoned;
	lptr[BACKUP_LEASES]    = &pool->backup;
	lptr[RESERVED_LEASES]  = &pool->reserved;

	for (i = FREE_LEASES; i <= RESERVED_LEASES; i++) 
	{
		/* If there's nothing on the queue, skip it. */
		if (!(LEASE_NOT_EMPTYP(lptr[i])))
			continue;

#if defined (FAILOVER_PROTOCOL)
		if (pool->failover_peer &&
		    pool->failover_peer->me.state != partner_down) {
			/*
			 * Normally the secondary doesn't initiate expiration
			 * events (unless in partner-down), but rather relies
			 * on the primary to expire the lease.  However, when
			 * disconnected from its peer, the server is allowed to
			 * rewind a lease to the previous state that the peer
			 * would have recorded it.  This means there may be
			 * opportunities for active->free or active->backup
			 * expirations while out of contact.
			 *
			 * Q: Should we limit this expiration to
			 *    comms-interrupt rather than not-normal?
			 */
			if ((i == ACTIVE_LEASES) &&
			    (pool->failover_peer->i_am == secondary) &&
			    (pool->failover_peer->me.state == normal))
				continue;

			/* Leases in an expired state don't move to
			   free because of a timeout unless we're in
			   partner_down. */
			if (i == EXPIRED_LEASES)
				continue;
		}
#endif		
		lease_reference(&lease, LEASE_GET_FIRSTP(lptr[i]), MDL);

		while (lease) 
		{
			/* Remember the next lease in the list. */
			if (next)
			{
				lease_dereference(&next, MDL);
			}
			
			ltemp = LEASE_GET_NEXTP(lptr[i], lease);
			if (ltemp)
			{
				lease_reference(&next, ltemp, MDL);
			}

			/* If we've run out of things to expire on this list, stop. */
			/* 链表是从小到大排序的，第一个比cur_time大的说明所有过期的都被处理完了 */
			if (lease->sort_time > cur_time) 
			{
				if (lease->sort_time < next_expiry)
					next_expiry = lease->sort_time;
				break;
			}

			/* If there is a pending state change, and
			   this lease has gotten to the time when the
			   state change should happen, just call
			   supersede_lease on it to make the change
			   happen. */
			/* 如某个lease到期了，且下个状态和本次状态不一样，要从链表中移动 */
			if (lease->next_binding_state != lease->binding_state)
			{
#if defined(FAILOVER_PROTOCOL)
				dhcp_failover_state_t *peer = NULL;

				if (lease->pool != NULL)
					peer = lease->pool->failover_peer;

				/* Can we rewind the lease to a free state? */
				if (peer != NULL &&
				    peer->service_state == not_cooperating &&
				    lease->next_binding_state == FTS_EXPIRED &&
				    ((peer->i_am == primary &&
				      lease->rewind_binding_state == FTS_FREE)
					||
				     (peer->i_am == secondary &&
				      lease->rewind_binding_state ==
								FTS_BACKUP)))
					lease->next_binding_state =
						   lease->rewind_binding_state;
#endif
				supersede_lease(lease, NULL, 1, 1, 1, 1);
			}

			lease_dereference(&lease, MDL);
			if (next)
				lease_reference(&lease, next, MDL);
		}
		if (next)
			lease_dereference(&next, MDL);
		if (lease)
			lease_dereference(&lease, MDL);
	}

	/* If we found something to expire and its expiration time
	 * is either less than the current expiration time or the
	 * current expiration time is already expired update the
	 * timer.
	 */
	if ((next_expiry != MAX_TIME) &&
	    ((pool->next_event_time > next_expiry) ||
	     (pool->next_event_time <= cur_time))) 
	{
		pool->next_event_time = next_expiry;
		tv.tv_sec = pool->next_event_time;
		tv.tv_usec = 0;
		add_timeout(&tv, pool_timer, pool,
			     (tvref_t)pool_reference,
			     (tvunref_t)pool_dereference);
	} 
	else
	{
		pool->next_event_time = MIN_TIME;
	}

}

/*********************************************************************
Func Name :   find_lease_by_ip_addr
Date Created: 2018/06/02
Author:  	  wangzhe
Description:  通过给定IP地址寻找lease
Input:	      IN struct lease ** lp		接收lease的指针
			  IN strcut iaddr addr		给定的IP地址
			  IN const char * file
			  IN int line
Output:       
Return:       void
Caution : 	  
*********************************************************************/
int find_lease_by_ip_addr 
(
	struct lease **lp, 
	struct iaddr addr,
	const char *file, 
	int line
)
{
	return lease_ip_hash_lookup(lp, lease_ip_addr_hash, addr.iabuf,
				    addr.len, file, line);
}

/*********************************************************************
Func Name :   find_lease_by_uid
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  通过给定uid查找lease
Input:	      IN struct lease ** lp		   接收lease的指针
			  IN const unsigned char * uid 给定的uid
			  IN unsigned len			   uid的长度
			  IN const char * file
			  IN int line
Output:       
Return:       void
Caution : 	  
*********************************************************************/
int find_lease_by_uid 
(
	struct lease **lp, 
	const unsigned char *uid,
	unsigned len, 
	const char *file, 
	int line
)
{
	if (len == 0)
		return 0;
	return lease_id_hash_lookup(lp, lease_uid_hash, uid, len, file, line);
}

/*********************************************************************
Func Name :   find_lease_by_hw_addr
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  通过给定硬件地址查找lease
Input:	      IN struct lease ** lp		   	  接收lease的指针
			  IN const unsigned char * hwaddr 给定硬件地址
			  IN unsigned len			   	  硬件地址的长度
			  IN const char * file
			  IN int line
Output:       
Return:       void
Caution : 	  
*********************************************************************/
int find_lease_by_hw_addr 
(
	struct lease **lp,
	const unsigned char *hwaddr, 
	unsigned hwlen,
	const char *file, 
	int line
)
{
	if (hwlen == 0)
		return (0);

	/*
	 * If it's an infiniband address don't bother
	 * as we don't have a useful address to hash.
	 */
	if ((hwlen == 1) && (hwaddr[0] == HTYPE_INFINIBAND))
		return (0);

	return (lease_id_hash_lookup(lp, lease_hw_addr_hash, hwaddr, hwlen,
				     file, line));
}

/*********************************************************************
Func Name :   client_lease_preferred
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  判断lease优先级是否比cand高
Input:	      IN struct lease * cand
			  IN struct lease * lease
Output:       
Return:       void
Caution : 	  If the lease is preferred over the candidate, return truth.  The
			  'cand' and 'lease' names are retained to read more clearly against
			  the 'uid_hash_add' and 'hw_hash_add' functions (this is common logic
			  to those two functions).
			 
			  1) ACTIVE leases are preferred.  The active lease with
			     the longest lifetime is preferred over shortest.
			  2) "transitional states" are next, this time with the
			     most recent CLTT.
			  3) free/backup/etc states are next, again with CLTT.  In truth we
			     should never see reset leases for this.
			  4) Abandoned leases are always dead last.
*********************************************************************/
static isc_boolean_t client_lease_preferred
(
	struct lease *cand, 
	struct lease *lease
)
{
	/* 若都是active的，时间长的优先 */
	if (cand->binding_state == FTS_ACTIVE) 
	{
		if (lease->binding_state == FTS_ACTIVE &&
		    lease->ends >= cand->ends)
			return ISC_TRUE;
	} 
	else if (cand->binding_state == FTS_EXPIRED ||
		   cand->binding_state == FTS_RELEASED) 
	{
		if (lease->binding_state == FTS_ACTIVE)
			return ISC_TRUE;
		/* 若都是过期状态或者释放状态，cltt大的优先 */
		if ((lease->binding_state == FTS_EXPIRED ||
		     lease->binding_state == FTS_RELEASED) &&
		    lease->cltt >= cand->cltt)
			return ISC_TRUE;
	} 
	else if (cand->binding_state != FTS_ABANDONED) 
	{
		if (lease->binding_state == FTS_ACTIVE ||
		    lease->binding_state == FTS_EXPIRED ||
		    lease->binding_state == FTS_RELEASED)
			return ISC_TRUE;

		if (lease->binding_state != FTS_ABANDONED &&
		    lease->cltt >= cand->cltt)
			return ISC_TRUE;
	} 
	else /* (cand->binding_state == FTS_ABANDONED) */ 
	{
		if (lease->binding_state != FTS_ABANDONED ||
		    lease->cltt >= cand->cltt)
			return ISC_TRUE;
	}

	return ISC_FALSE;
}

/*********************************************************************
Func Name :   uid_hash_add
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将lease加入uid的hash表
Input:	      IN struct lease * lease
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void uid_hash_add
(
	struct lease *lease
)
{
	struct lease *head = NULL;
	struct lease *cand = NULL;
	struct lease *prev = NULL;
	struct lease *next = NULL;

	/* 在hash表中查找，若没找到，直接添加 */
	if (!find_lease_by_uid(&head, lease->uid, lease->uid_len, MDL))
	{
		lease_id_hash_add(lease_uid_hash, lease->uid, lease->uid_len,
				  lease, MDL);
	}
	else 
	{
		/* Otherwise, insert it into the list in order of its
		 * preference for "resuming allocation to the client."
		 *
		 * Because we don't have control of the hash bucket index
		 * directly, we have to remove and re-insert the client
		 * id into the hash if we're inserting onto the head.
		 */
		lease_reference(&cand, head, MDL);
		while (cand != NULL) 
		{
			/* 若lease的优先级更高 */
			if (client_lease_preferred(cand, lease))
				break;

			if (prev != NULL)
				lease_dereference(&prev, MDL);
			lease_reference(&prev, cand, MDL);

			if (cand->n_uid != NULL)
				lease_reference(&next, cand->n_uid, MDL);

			lease_dereference(&cand, MDL);

			if (next != NULL) {
				lease_reference(&cand, next, MDL);
				lease_dereference(&next, MDL);
			}
		}

		/* If we want to insert 'before cand', and prev is NULL,
		 * then it was the head of the list.  Assume that position.
		 */
		if (prev == NULL) 
		{
			/* 要确保hash中找到的uid_lease是链表头，所以若lease是头，那么要把原来的head删掉，
			换成lease插入hash表 */
			lease_reference(&lease->n_uid, head, MDL);
			lease_id_hash_delete(lease_uid_hash, lease->uid,
					     lease->uid_len, MDL);
			lease_id_hash_add(lease_uid_hash, lease->uid,
					  lease->uid_len, lease, MDL);
		} 
		else /* (prev != NULL) */ 
		{
			/* 把lease插入pre后面 */
			if (prev->n_uid != NULL) 
			{
				lease_reference(&lease->n_uid, prev->n_uid, MDL);
				lease_dereference(&prev->n_uid, MDL);
			}
			lease_reference(&prev->n_uid, lease, MDL);
			lease_dereference(&prev, MDL);
		}

		if (cand != NULL)
			lease_dereference(&cand, MDL);
		lease_dereference(&head, MDL);
	}
}

/*********************************************************************
Func Name :   uid_hash_delete
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  在uid的hash表中删除lease
Input:	      IN struct lease * lease
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void uid_hash_delete 
(
	struct lease * lease
)
{
	struct lease *head = (struct lease *)0;
	struct lease *scan;

	/* If it's not in the hash, we have no work to do. */
	if (!find_lease_by_uid(&head, lease->uid, lease->uid_len, MDL)) 
	{
		if (lease->n_uid)
			lease_dereference(&lease->n_uid, MDL);
		return;
	}

	/* If the lease we're freeing is at the head of the list,
	   remove the hash table entry and add a new one with the
	   next lease on the list (if there is one). */
	if (head == lease) 
	{
		lease_id_hash_delete(lease_uid_hash, lease->uid, lease->uid_len, MDL);
		/* 若lease有next，将lease的next先insert再delete，达到将next放到表头的目的 */
		if (lease->n_uid) 
		{
			lease_id_hash_add(lease_uid_hash, lease->n_uid->uid,
					  lease->n_uid->uid_len, lease->n_uid, MDL);
			lease_dereference(&lease->n_uid, MDL);
		}
	} 
	else 
	{
		/* Otherwise, look for the lease in the list of leases
		   attached to the hash table entry, and remove it if
		   we find it. */
		for (scan = head; scan->n_uid; scan = scan->n_uid) 
		{
			if (scan->n_uid == lease) 
			{
				lease_dereference(&scan->n_uid, MDL);
				if (lease->n_uid) 
				{
					lease_reference(&scan->n_uid, lease->n_uid, MDL);
					lease_dereference(&lease->n_uid, MDL);
				}
				break;
			}
		}
	}
	lease_dereference(&head, MDL);
}

/*********************************************************************
Func Name :   hw_hash_add
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将lease插入硬件的hash表中
Input:	      IN struct lease * lease
Output:       
Return:       void
Caution : 	  Add the specified lease to the hardware address hash.
				 We don't add leases with infiniband addresses to the
				 hash as there isn't any address to hash on.
*********************************************************************/
void hw_hash_add
(
	struct lease *lease
)
{
	struct lease *head = NULL;
	struct lease *cand = NULL;
	struct lease *prev = NULL;
	struct lease *next = NULL;

	/*
	 * If it's an infiniband address don't bother
	 * as we don't have a useful address to hash.
	 */
	if ((lease->hardware_addr.hlen == 1) &&
	    (lease->hardware_addr.hbuf[0] == HTYPE_INFINIBAND))
		return;
	   
	/* 先在hash表中查找，若没找到，直接add */
	if (!find_lease_by_hw_addr(&head, lease->hardware_addr.hbuf,
				    lease->hardware_addr.hlen, MDL))
	{
		lease_id_hash_add(lease_hw_addr_hash,
				  lease->hardware_addr.hbuf,
				  lease->hardware_addr.hlen, lease, MDL);
	}
	else 
	{
		/* Otherwise, insert it into the list in order of its
		 * preference for "resuming allocation to the client."
		 *
		 * Because we don't have control of the hash bucket index
		 * directly, we have to remove and re-insert the client
		 * id into the hash if we're inserting onto the head.
		 */

		/* 将hash表头挂接给cand */
		lease_reference(&cand, head, MDL);
		while (cand != NULL) 
		{
			if (client_lease_preferred(cand, lease))
				break;

			if (prev != NULL)
				lease_dereference(&prev, MDL);
			lease_reference(&prev, cand, MDL);

			if (cand->n_hw != NULL)
				lease_reference(&next, cand->n_hw, MDL);

			lease_dereference(&cand, MDL);

			if (next != NULL) 
			{
				lease_reference(&cand, next, MDL);
				lease_dereference(&next, MDL);
			}
		}

		/* If we want to insert 'before cand', and prev is NULL,
		 * then it was the head of the list.  Assume that position.
		 */
		if (prev == NULL) 
		{
			lease_reference(&lease->n_hw, head, MDL);
			lease_id_hash_delete(lease_hw_addr_hash,
					     lease->hardware_addr.hbuf,
					     lease->hardware_addr.hlen, MDL);
			lease_id_hash_add(lease_hw_addr_hash,
					  lease->hardware_addr.hbuf,
					  lease->hardware_addr.hlen,
					  lease, MDL);
		} 
		else /* (prev != NULL) */ 
		{
			if(prev->n_hw != NULL) 
			{
				lease_reference(&lease->n_hw, prev->n_hw, MDL);
				lease_dereference(&prev->n_hw, MDL);
			}
			lease_reference(&prev->n_hw, lease, MDL);
			lease_dereference(&prev, MDL);
		}

		if (cand != NULL)
			lease_dereference(&cand, MDL);
		lease_dereference(&head, MDL);
	}
}

/*********************************************************************
Func Name :   hw_hash_delete
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  从硬件hash表中删除lease
Input:	      IN struct lease * lease
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void hw_hash_delete 
(
	struct lease * lease
)
{
	struct lease *head = (struct lease *)0;
	struct lease *next = (struct lease *)0;

	/*
	 * If it's an infiniband address don't bother
	 * as we don't have a useful address to hash.
	 */
	if ((lease->hardware_addr.hlen == 1) &&
	    (lease->hardware_addr.hbuf[0] == HTYPE_INFINIBAND))
		return;

	/* 在hash表中查找lease，若没找到，直接return */
	if (!find_lease_by_hw_addr(&head, lease->hardware_addr.hbuf,
				    lease->hardware_addr.hlen, MDL)) 
	{
		if (lease->n_hw)
			lease_dereference(&lease->n_hw, MDL);
		return;
	}

	/* If the lease we're freeing is at the head of the list,
	   remove the hash table entry and add a new one with the
	   next lease on the list (if there is one). */
	if (head == lease) 
	{
		lease_id_hash_delete(lease_hw_addr_hash,
				     lease->hardware_addr.hbuf,
				     lease->hardware_addr.hlen, MDL);
		if (lease->n_hw) 
		{
			lease_id_hash_add(lease_hw_addr_hash,
					  lease->n_hw->hardware_addr.hbuf,
					  lease->n_hw->hardware_addr.hlen,
					  lease->n_hw, MDL);
			lease_dereference(&lease->n_hw, MDL);
		}
	} 
	else 
	{
		/* Otherwise, look for the lease in the list of leases
		   attached to the hash table entry, and remove it if
		   we find it. */
		while (head->n_hw) 
		{
			if (head->n_hw == lease) 
			{
				lease_dereference(&head->n_hw, MDL);
				if (lease->n_hw) 
				{
					lease_reference(&head->n_hw, lease->n_hw, MDL);
					lease_dereference(&lease->n_hw, MDL);
				}
				break;
			}
			lease_reference(&next, head->n_hw, MDL);
			lease_dereference(&head, MDL);
			lease_reference(&head, next, MDL);
			lease_dereference(&next, MDL);
		}
	}
	
	if (head)
		lease_dereference(&head, MDL);
}

/*********************************************************************
Func Name :   write_leases4
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将V4的leases写入永久保存介质
Input:	      void
Output:       
Return:       
Caution : 	  Write v4 leases to permanent storage
*********************************************************************/
int write_leases4(void) 
{
	struct lease *l;
	struct shared_network *s;
	struct pool *p;
	LEASE_STRUCT_PTR lptr[RESERVED_LEASES + 1];
	int num_written = 0;			//记录一共写入了多少个lease
	int i;

	/* 循环shared_networks链表 */
	for (s = shared_networks; s; s = s->next) 
	{
		/* 循环一个shared_networks的pools */
	    for (p = s->pools; p; p = p->next) 
		{
			lptr[FREE_LEASES] 	   = &p->free;
			lptr[ACTIVE_LEASES]    = &p->active;
			lptr[EXPIRED_LEASES]   = &p->expired;
			lptr[ABANDONED_LEASES] = &p->abandoned;
			lptr[BACKUP_LEASES]    = &p->backup;
			lptr[RESERVED_LEASES]  = &p->reserved;

			/* 循环一个pool中每种租约状态对应的链表 */
			for (i = FREE_LEASES; i <= RESERVED_LEASES; i++) 
			{
				/* 循环一个链表 */
		    	for (l = LEASE_GET_FIRSTP(lptr[i]); l != NULL; l = LEASE_GET_NEXTP(lptr[i], l)) 
				{
#if !defined (DEBUG_DUMP_ALL_LEASES)
					if (l->hardware_addr.hlen != 0 || l->uid_len != 0 ||
			    		l->tsfp != 0 || l->binding_state != FTS_FREE)
#endif
					{
			    		if (write_lease(l) == 0)
				    		return (0);
			    		num_written++;
					}
		    	}
			}
	    }
	}

	log_info ("Wrote %d leases to leases file.", num_written);
	return (1);
}

/*********************************************************************
Func Name :   write_leases
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将所有leases写入永久性存储介质
Input:	      void
Output:       
Return:       int
Caution : 	  Write v4 leases to permanent storage
*********************************************************************/
int write_leases ()
{
	struct host_decl *hp;
	struct group_object *gp;
	struct hash_bucket *hb;
	struct class *cp;
	struct collection *colp;
	int i;
	int num_written;

	/* write all the dynamically-created class declarations. */
	if (collections->classes) 
	{
		numclasseswritten = 0;
		for (colp = collections ; colp; colp = colp->next) 
		{
			for (cp = colp->classes; cp; cp = cp->nic) 
			{
				write_named_billing_class((unsigned char *)cp->name, 0, cp);
			}
		}

		/* XXXJAB this number doesn't include subclasses... */ 
		log_info("Wrote %d class decls to leases file.", numclasseswritten);
	}
	
			
	/* Write all the dynamically-created group declarations. */
	if (group_name_hash) 
	{
	    num_written = 0;
	    for (i = 0; i < group_name_hash -> hash_count; i++) 
		{
			for (hb = group_name_hash -> buckets[i]; hb; hb = hb->next) 
			{
				gp = (struct group_object *)hb->value;
				if ((gp->flags & GROUP_OBJECT_DYNAMIC) ||
				    ((gp->flags & GROUP_OBJECT_STATIC) &&
				     (gp->flags & GROUP_OBJECT_DELETED))) 
				{
					if (!write_group(gp))
						return 0;
					++num_written;
				}
			}
	    }
	    log_info("Wrote %d group decls to leases file.", num_written);
	}

	/* Write all the deleted host declarations. */
	if (host_name_hash) 
	{
	    num_written = 0;
	    for (i = 0; i < host_name_hash->hash_count; i++) 
		{
			for (hb = host_name_hash->buckets[i]; hb; hb = hb->next) 
			{
				hp = (struct host_decl *)hb->value;
				if (((hp->flags & HOST_DECL_STATIC) &&
				     (hp->flags & HOST_DECL_DELETED))) 
				{
					if (!write_host(hp))
						return 0;
					++num_written;
				}
			}
	    }
	    log_info("Wrote %d deleted host decls to leases file.",
		      num_written);
	}

	/* Write all the new, dynamic host declarations. */
	if (host_name_hash) 
	{
	    num_written = 0;
	    for (i = 0; i < host_name_hash->hash_count; i++) 
		{
			for (hb = host_name_hash->buckets[i]; hb; hb = hb->next) 
			{
				hp = (struct host_decl *)hb->value;
				if ((hp->flags & HOST_DECL_DYNAMIC)) 
				{
					if (!write_host(hp))
						++num_written;
				}
			}
	    }
	    log_info("Wrote %d new dynamic host decls to leases file.",
		      num_written);
	}

#if defined (FAILOVER_PROTOCOL)
	/* Write all the failover states. */
	if (!dhcp_failover_write_all_states ())
		return 0;
#endif

	switch (local_family) {
	      case AF_INET:
		if (write_leases4() == 0)
			return (0);
		break;
#ifdef DHCPv6
	      case AF_INET6:
		if (write_leases6() == 0)
			return (0);
		break;
#endif /* DHCPv6 */
	}

	if (commit_leases() == 0)
		return (0);
	return (1);
}

#if !defined (BINARY_LEASES)

/*********************************************************************
Func Name :   lease_remove_all
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将lq中所有leases删除
Input:	      IN struct lease ** lq
Output:       
Return:       
Caution : 	  Unlink all the leases in the queue
*********************************************************************/
void lease_remove_all
(
	struct lease **lq
) 
{
	struct lease *lp, *ln = NULL;

	/* nothing to do */
	if (*lq == NULL)
		return;

	/* We simply derefernce the first item in the list.  When
	 * it's reference counter goes to zero it will be cleaned
	 * and the reference counter
	 *
	 * Get a pointer to the first item in the list and then
	 * drop the reference from the queue pointer
	 */
	lease_reference(&lp, *lq, MDL);
	lease_dereference(lq, MDL);

	/* 循环链表所有节点 */
	do {
		/* if we have a next save a pointer to it and unlink it */
		if (lp->next) 
		{
			lease_reference(&ln, lp->next, MDL);
			lease_dereference(&lp->next, MDL);
		}

		/* get rid of what we currently have */
		lease_dereference(&lp, MDL);

		/* move the next to the current and loop */
		lp = ln;
		ln = NULL;
	} while (lp != NULL);
}

/*********************************************************************
Func Name :   lease_remove
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  从lq链表中删除comp
Input:	      IN struct lease ** lq
			  IN struct lease * comp
Output:       
Return:       
Caution : 	  This routine walks through a given lease queue (lq) looking
				for comp.  If it doesn't find the lease it is a fatal error
				as it should be on the given queue.  Once we find the lease
				we can remove it from this list
*********************************************************************/
void lease_remove
(
	struct lease **lq, 
	struct lease *comp
)
{
	struct lease *prev, *lp;

	prev = NULL;
	for (lp = *lq; lp != NULL; lp = lp->next) 
	{
		if (lp == comp)
			break;
		prev = lp;
	}

	if (!lp) 
	{
		log_fatal("Lease with binding state %s not on its queue.",
			  (comp->binding_state < 1 ||
			   comp->binding_state > FTS_LAST)
			  ? "unknown"
			  : binding_state_names[comp->binding_state - 1]);
	}

	if (prev) 
	{
		lease_dereference(&prev->next, MDL);
		if (comp->next) 
		{
			lease_reference(&prev->next, comp->next, MDL);
			lease_dereference(&comp->next, MDL);
		}
	} 
	else 
	{
		lease_dereference(lq, MDL);
		if (comp->next) 
		{
			lease_reference(lq, comp->next, MDL);
			lease_dereference(&comp->next, MDL);
		}
	}
}

/* This routine inserts comp into lq in a sorted fashion.
 * The sort key is comp->sort_time, smaller values are
 * placed earlier in the list.
 */

/*********************************************************************
Func Name :   lease_insert
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将comp按照顺序插入lq链表中
Input:	      IN struct lease ** lq
			  IN struct lease * comp
Output:       
Return:       
Caution : 	  This routine inserts comp into lq in a sorted fashion.
			  The sort key is comp->sort_time, smaller values are
			  placed earlier in the list
*********************************************************************/
void lease_insert
(
	struct lease **lq, 
	struct lease *comp
)
{
	struct lease *prev, *lp;
	static struct lease **last_lq = NULL;
	static struct lease *last_insert_point = NULL;

	/* This only works during server startup: during runtime, the last
	 * lease may be dequeued in between calls.  If the queue is the same
	 * as was used previously, and the lease structure isn't (this is not
	 * a re-queue), use that as a starting point for the insertion-sort.
	 */
	if ((server_starting & SS_QFOLLOW) && (lq == last_lq) &&
	    (comp != last_insert_point) && 
	    (last_insert_point->sort_time <= comp->sort_time)) 
	{
		prev = last_insert_point;
		lp = prev->next;
	} 
	else 
	{
		prev = NULL;
		lp = *lq;
	}

	/* Insertion sort the lease onto the appropriate queue. */
	for (; lp != NULL; lp = lp->next) 
	{
		if (lp->sort_time >= comp->sort_time)
			break;
		prev = lp;
	}

	if (prev) 
	{
		if (prev->next) 
		{
			lease_reference(&comp->next, prev->next, MDL);
			lease_dereference(&prev->next, MDL);
		}
		lease_reference(&prev->next, comp, MDL);
	} 
	else 
	{
		if (*lq) 
		{
			lease_reference(&comp->next, *lq, MDL);
			lease_dereference(lq, MDL);
		}
		lease_reference(lq, comp, MDL);
	}
	last_insert_point = comp;
	last_lq = lq;

	return;
}
#endif

/* In addition to placing this lease upon a lease queue depending on its
 * state, it also keeps track of the number of FREE and BACKUP leases in
 * existence, and sets the sort_time on the lease.
 *
 * Sort_time is used in pool_timer() to determine when the lease will
 * bubble to the top of the list and be supersede_lease()'d into its next
 * state (possibly, if all goes well).  Example, ACTIVE leases move to
 * EXPIRED state when the 'ends' value is reached, so that is its sort
 * time.  Most queues are sorted by 'ends', since it is generally best
 * practice to re-use the oldest lease, to reduce address collision
 * chances.
 */

/*********************************************************************
Func Name :   lease_enqueue
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  根据lease的状态，将lease加入到pool下对应的队列中，并设置sort_time作为key
Input:	      IN struct lease * comp
Output:       
Return:       
Caution : 	  In addition to placing this lease upon a lease queue depending on its
			  state, it also keeps track of the number of FREE and BACKUP leases in
			  existence, and sets the sort_time on the lease.
			  
			  Sort_time is used in pool_timer() to determine when the lease will
			  bubble to the top of the list and be supersede_lease()'d into its next
			  state (possibly, if all goes well).  Example, ACTIVE leases move to
			  EXPIRED state when the 'ends' value is reached, so that is its sort
			  time.  Most queues are sorted by 'ends', since it is generally best
			  practice to re-use the oldest lease, to reduce address collision
			  chances
*********************************************************************/
int lease_enqueue 
(
	struct lease *comp
)
{
	LEASE_STRUCT_PTR lq;  

	/* No queue to put it on? */
	if (!comp->pool)
		return 0;

	/* Figure out which queue it's going to. */
	switch (comp->binding_state) {
	      case FTS_FREE:
		if (comp->flags & RESERVED_LEASE) 
		{
			lq = &comp->pool->reserved;
		} 
		else 
		{
			lq = &comp->pool->free;
			comp->pool->free_leases++;
		}
		comp->sort_time = comp->ends;
		break;

	      case FTS_ACTIVE:
		lq = &comp->pool->active;
		comp->sort_time = comp->ends;
		break;

	      case FTS_EXPIRED:
	      case FTS_RELEASED:
	      case FTS_RESET:
		lq = &comp->pool->expired;
#if defined(FAILOVER_PROTOCOL)
		/* In partner_down, tsfp is the time at which the lease
		 * may be reallocated (stos+mclt).  We can do that with
		 * lease_mine_to_reallocate() anywhere between tsfp and
		 * ends.  But we prefer to wait until ends before doing it
		 * automatically (choose the greater of the two).  Note
		 * that 'ends' is usually a historic timestamp in the
		 * case of expired leases, is really only in the future
		 * on released leases, and if we know a lease to be released
		 * the peer might still know it to be active...in which case
		 * it's possible the peer has renewed this lease, so avoid
		 * doing that.
		 */
		if (comp->pool->failover_peer &&
		    comp->pool->failover_peer->me.state == partner_down)
			comp->sort_time = (comp->tsfp > comp->ends) ?
					  comp->tsfp : comp->ends;
		else
#endif
			comp->sort_time = comp->ends;

		break;

	      case FTS_ABANDONED:
		lq = &comp->pool->abandoned;
		comp->sort_time = comp->ends;
		break;

	      case FTS_BACKUP:
		if (comp->flags & RESERVED_LEASE) 
		{
			lq = &comp->pool->reserved;
		} 
		else 
		{
			lq = &comp->pool->backup;
			comp->pool->backup_leases++;
		}
		comp->sort_time = comp->ends;
		break;

	      default:
		log_error ("Lease with bogus binding state: %d",
			   comp->binding_state);
#if defined (BINDING_STATE_DEBUG)
		abort ();
#endif
		return 0;
	}

	LEASE_INSERTP(lq, comp);

	return 1;
}

/*********************************************************************
Func Name :   lease_instantiate
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  将object实例化为lease并加入相关hash表
Input:	      IN struct lease * comp
Output:       
Return:       
Caution : 	  For a given lease, sort it onto the right list in its pool and put it
			   in each appropriate hash, understanding that it's already by definition
			   in lease_ip_addr_hash
*********************************************************************/
isc_result_t lease_instantiate
(
	const void *key, 
	unsigned len, 
	void *object
)
{
	struct lease *lease = object;
	struct class *class;
	/* XXX If the lease doesn't have a pool at this point, it's an
	   XXX orphan, which we *should* keep around until it expires,
	   XXX but which right now we just forget. */
	if (!lease->pool) 
	{
		lease_ip_hash_delete(lease_ip_addr_hash, lease->ip_addr.iabuf,
				     lease->ip_addr.len, MDL);
		return ISC_R_SUCCESS;
	}

#if defined (FAILOVER_PROTOCOL)
	/* If the lease is in FTS_BACKUP but there is no peer, then the
 	 * pool must have been formerly configured for failover and
 	 * is now configured as standalone. This means we need to
 	 * move the lease to FTS_FREE to make it available. */
	if ((lease->binding_state == FTS_BACKUP) && 
	    (lease->pool->failover_peer == NULL)) {
#else
	/* We aren't compiled for failover, so just move to FTS_FREE */
	if (lease->binding_state == FTS_BACKUP) { 
#endif
		lease->binding_state = FTS_FREE;
		lease->next_binding_state = FTS_FREE;
		lease->rewind_binding_state = FTS_FREE;
	}
		
	/* Put the lease on the right queue.  Failure to queue is probably
	 * due to a bogus binding state.  In such a case, we claim success,
	 * so that later leases in a hash_foreach are processed, but we
	 * return early as we really don't want hw address hash entries or
	 * other cruft to surround such a bogus entry.
	 */
	if (!lease_enqueue(lease))
		return ISC_R_SUCCESS;

	/* Record the lease in the uid hash if possible. */
	if (lease->uid) 
	{
		uid_hash_add(lease);
	}

	/* Record it in the hardware address hash if possible. */
	if (lease->hardware_addr.hlen) 
	{
		hw_hash_add(lease);
	}

	/* If the lease has a billing class, set up the billing. */
	if (lease->billing_class) 
	{
		class = (struct class *)0;
		class_reference(&class, lease->billing_class, MDL);
		class_dereference(&lease->billing_class, MDL);
		/* If the lease is available for allocation, the billing
		   is invalid, so we don't keep it. */
		if (lease->binding_state == FTS_ACTIVE ||
		    lease->binding_state == FTS_EXPIRED ||
		    lease->binding_state == FTS_RELEASED ||
		    lease->binding_state == FTS_RESET)
			bill_class(lease, class);
		class_dereference (&class, MDL);
	}
	return ISC_R_SUCCESS;
}

/*********************************************************************
Func Name :   expire_all_pools
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  在所有pool上运行到期事件，只在初始化时调用
Input:	      void
Output:       
Return:       void
Caution : 	  Run expiry events on every pool.   This is called on startup so that
			   any expiry events that occurred after the server stopped and before it
			   was restarted can be run.   At the same time, if failover support is
			   compiled in, we compute the balance of leases for the pool
*********************************************************************/
void expire_all_pools()
{
	struct shared_network *s;
	struct pool *p;
	int i;
	struct lease *l;
	LEASE_STRUCT_PTR lptr[RESERVED_LEASES + 1];

	/* Indicate that we are in the startup phase */
	server_starting = SS_NOSYNC | SS_QFOLLOW;

#if defined (BINARY_LEASES)
	/* set up the growth factors for the binary leases.
	 * We use 100% for free, 50% for active and backup
	 * 20% for expired, abandoned and reserved
	 * but no less than 100, 50, and 20.
	 */
	for (s = shared_networks; s; s = s->next) 
	{
	    for (p = s->pools; p != NULL; p = p->next) 
		{
			size_t num_f = 100, num_a = 50, num_e = 20;
			if (p->lease_count > 100) 
			{
			    num_f = p->lease_count;
			    num_a = num_f / 2;
			    num_e = num_f / 5;
			}
			lc_init_growth(&p->free, num_f);
			lc_init_growth(&p->active, num_a);
			lc_init_growth(&p->expired, num_a);
			lc_init_growth(&p->abandoned, num_e);
			lc_init_growth(&p->backup, num_e);
			lc_init_growth(&p->reserved, num_e);
	    }
	}
#endif

	/* First, go over the hash list and actually put all the leases
	   on the appropriate lists. */
	   /* hash_foreach，顺序遍历hash表中的每个元素，并对其调用函数 */
	lease_ip_hash_foreach(lease_ip_addr_hash, lease_instantiate);

	/* Loop through each pool in each shared network and call the
	 * expiry routine on the pool.  It is no longer safe to follow
	 * the queue insertion point, as expiration of a lease can move
	 * it between queues (and this may be the lease that function
	 * points at).
	 */
	server_starting &= ~SS_QFOLLOW;
	for (s = shared_networks; s; s = s->next) 
	{
	    for (p = s->pools; p; p = p->next) 
		{
			pool_timer(p);

			p->lease_count = 0;
			p->free_leases = 0;
			p->backup_leases = 0;

			lptr[FREE_LEASES] 	   = &p->free;
			lptr[ACTIVE_LEASES]    = &p->active;
			lptr[EXPIRED_LEASES]   = &p->expired;
			lptr[ABANDONED_LEASES] = &p->abandoned;
			lptr[BACKUP_LEASES]    = &p->backup;
			lptr[RESERVED_LEASES]  = &p->reserved;

			for (i = FREE_LEASES; i <= RESERVED_LEASES; i++) 
			{
			    for (l = LEASE_GET_FIRSTP(lptr[i]); l != NULL; l = LEASE_GET_NEXTP(lptr[i], l)) 
				{
					p->lease_count++;
					if (l->ends <= cur_time) 
					{
						if (l->binding_state == FTS_FREE) 
						{
							if (i == FREE_LEASES)
								p->free_leases++;
							else if (i != RESERVED_LEASES)
								log_fatal("Impossible case "
									  "at %s:%d.", MDL);
						} 
						else if (l->binding_state == FTS_BACKUP) 
						{
							if (i == BACKUP_LEASES)
								p->backup_leases++;
							else if (i != RESERVED_LEASES)
								log_fatal("Impossible case "
									  "at %s:%d.", MDL);
						}
					}
#if defined (FAILOVER_PROTOCOL)
					if (p->failover_peer && l->tstp > l->atsfp && !(l->flags & ON_UPDATE_QUEUE)) 
					{
						l->desired_binding_state = l->binding_state;
						dhcp_failover_queue_update (l, 1);
					}
#endif
			    }
			}
	    }
	}

	/* turn off startup phase */
	server_starting = 0;
}

/*********************************************************************
Func Name :   dump_subnets
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  这个函数是用来打印信息的
Input:	      void
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void dump_subnets()
{
	struct lease *l;
	struct shared_network *s;
	struct subnet *n;
	struct pool *p;
	LEASE_STRUCT_PTR lptr[RESERVED_LEASES + 1];
	int i;

	log_info("Subnets:");
	for (n = subnets; n; n = n->next_subnet) 
	{
		log_debug ("  Subnet %s", piaddr(n->net));
		log_debug ("     netmask %s",
		       piaddr (n -> netmask));
	}
	log_info ("Shared networks:");
	for (s = shared_networks; s; s = s->next) 
	{
	    log_info("  %s", s -> name);
	    for (p = s->pools; p; p = p->next) 
		{
			lptr[FREE_LEASES]      = &p->free;
			lptr[ACTIVE_LEASES]    = &p->active;
			lptr[EXPIRED_LEASES]   = &p->expired;
			lptr[ABANDONED_LEASES] = &p->abandoned;
			lptr[BACKUP_LEASES]    = &p->backup;
			lptr[RESERVED_LEASES]  = &p->reserved;

			for (i = FREE_LEASES; i <= RESERVED_LEASES; i++) 
			{
			    for (l = LEASE_GET_FIRSTP(lptr[i]); l != NULL; l = LEASE_GET_NEXTP(lptr[i], l)) 
				{
				    print_lease(l);
			    }
			}
	    }
	}
}

HASH_FUNCTIONS(lease_ip, const unsigned char *, struct lease, lease_ip_hash_t,
	       lease_reference, lease_dereference, do_ip4_hash)
HASH_FUNCTIONS(lease_id, const unsigned char *, struct lease, lease_id_hash_t,
	       lease_reference, lease_dereference, do_id_hash)
HASH_FUNCTIONS (host, const unsigned char *, struct host_decl, host_hash_t,
		host_reference, host_dereference, do_string_hash)
HASH_FUNCTIONS (class, const char *, struct class, class_hash_t,
		class_reference, class_dereference, do_string_hash)

#if defined (DEBUG_MEMORY_LEAKAGE) && \
		defined (DEBUG_MEMORY_LEAKAGE_ON_EXIT)
extern struct hash_table *dns_zone_hash;
extern struct interface_info **interface_vector;
extern int interface_count;
dhcp_control_object_t *dhcp_control_object;
extern struct hash_table *auth_key_hash;
struct hash_table *universe_hash;
struct universe **universes;
int universe_count, universe_max;
#if 0
extern int end;
#endif

#if defined (COMPACT_LEASES)
extern struct lease *lease_hunks;
#endif

/*********************************************************************
Func Name :   free_everything
Date Created: 2018/06/04
Author:  	  wangzhe
Description:  这个函数测试内存的时候使用，释放所有相关内存
Input:	      void
Output:       
Return:       void
Caution : 	  
*********************************************************************/
void free_everything(void)
{
	struct subnet *sc = (struct subnet *)0, *sn = (struct subnet *)0;
	struct shared_network *nc = (struct shared_network *)0,
		*nn = (struct shared_network *)0;
	struct pool *pc = (struct pool *)0, *pn = (struct pool *)0;
	struct lease *lc = NULL, *ln = NULL,  *ltemp = NULL;
	struct interface_info *ic = (struct interface_info *)0,
		*in = (struct interface_info *)0;
	struct class *cc = (struct class *)0, *cn = (struct class *)0;
	struct collection *lp;
	int i;

	/* Get rid of all the hash tables. */
	if (host_hw_addr_hash)
		host_free_hash_table(&host_hw_addr_hash, MDL);
	host_hw_addr_hash = 0;
	
	if (host_uid_hash)
		host_free_hash_table(&host_uid_hash, MDL);
	host_uid_hash = 0;
	
	if (lease_uid_hash)
		lease_id_free_hash_table(&lease_uid_hash, MDL);
	lease_uid_hash = 0;
	
	if (lease_ip_addr_hash)
		lease_ip_free_hash_table(&lease_ip_addr_hash, MDL);
	lease_ip_addr_hash = 0;
	
	if (lease_hw_addr_hash)
		lease_id_free_hash_table(&lease_hw_addr_hash, MDL);
	lease_hw_addr_hash = 0;
	
	if (host_name_hash)
		host_free_hash_table(&host_name_hash, MDL);
	host_name_hash = 0;
	
	if (dns_zone_hash)
		dns_zone_free_hash_table(&dns_zone_hash, MDL);
	dns_zone_hash = 0;

	while (host_id_info != NULL) 
	{
		host_id_info_t *tmp;
		option_dereference(&host_id_info->option, MDL);
		host_free_hash_table(&host_id_info->values_hash, MDL);
		tmp = host_id_info->next;
		dfree(host_id_info, MDL);
		host_id_info = tmp;
	}
#if 0
	if (auth_key_hash)
		auth_key_free_hash_table (&auth_key_hash, MDL);
#endif
	auth_key_hash = 0;

	omapi_object_dereference((omapi_object_t **)&dhcp_control_object, MDL);

	/* 清理全局class配置 */
	for (lp = collections; lp; lp = lp->next) 
	{
	    if (lp->classes) 
		{
			class_reference(&cn, lp->classes, MDL);
			do {
			    if (cn) 
				{
					class_reference(&cc, cn, MDL);
					class_dereference(&cn, MDL);
			    }
			    if (cc->nic) 
				{
					class_reference(&cn, cc->nic, MDL);
					class_dereference(&cc->nic, MDL);
			    }
			    group_dereference(&cc->group, MDL);
			    if (cc->hash) 
				{
				    class_free_hash_table(&cc->hash, MDL);
				    cc->hash = (struct hash_table *)0;
			    }
			    class_dereference(&cc, MDL);
			} while (cn);
			
			class_dereference (&lp->classes, MDL);
	    }
	}

	if (interface_vector) 
	{
	    for (i = 0; i < interface_count; i++) 
		{
			if (interface_vector[i])
			    interface_dereference(&interface_vector[i], MDL);
	    }
	    dfree(interface_vector, MDL);
	    interface_vector = 0;
	}

	if (interfaces) 
	{
	    interface_reference(&in, interfaces, MDL);
	    do {
			if (in) 
			{
			    interface_reference(&ic, in, MDL);
			    interface_dereference(&in, MDL);
			}
			
			if (ic->next) 
			{
			    interface_reference(&in, ic->next, MDL);
			    interface_dereference(&ic->next, MDL);
			}
			
			omapi_unregister_io_object((omapi_object_t *)ic);
			if (ic->shared_network) 
			{
			    if (ic->shared_network->interface)
				    interface_dereference(&ic->shared_network->interface, MDL);
			    shared_network_dereference(&ic->shared_network, MDL);
			}
			
			interface_dereference(&ic, MDL);
	    } while (in);
	    interface_dereference(&interfaces, MDL);
	}

	/* Subnets are complicated because of the extra links. */
	if (subnets) 
	{
	    subnet_reference(&sn, subnets, MDL);
	    do {
			if (sn) 
			{
			    subnet_reference(&sc, sn, MDL);
			    subnet_dereference(&sn, MDL);
			}
			
			if (sc->next_subnet) 
			{
			    subnet_reference(&sn, sc->next_subnet, MDL);
			    subnet_dereference(&sc->next_subnet, MDL);
			}
			
			if (sc->next_sibling)
			    subnet_dereference(&sc->next_sibling, MDL);
			if (sc->shared_network)
			    shared_network_dereference(&sc->shared_network, MDL);
			group_dereference(&sc->group, MDL);
			if (sc->interface)
			    interface_dereference(&sc->interface, MDL);
			subnet_dereference(&sc, MDL);
	    } while (sn);
	    subnet_dereference(&subnets, MDL);
	}

	/* So are shared networks. */
	/* XXX: this doesn't work presently, but i'm ok just filtering
	 * it out of the noise (you get a bigger spike on the real leaks).
	 * It would be good to fix this, but it is not a "real bug," so not
	 * today.  This hack is incomplete, it doesn't trim out sub-values.
	 */
	if (shared_networks) 
	{
		shared_network_dereference (&shared_networks, MDL);
	/* This is the old method (tries to free memory twice, broken) */
	} 
	else if (0) 
	{
	    shared_network_reference(&nn, shared_networks, MDL);
	    do {
			if (nn) 
			{
			    shared_network_reference(&nc, nn, MDL);
			    shared_network_dereference(&nn, MDL);
			}
			
			if(nc->next) 
			{
			    shared_network_reference(&nn, nc->next, MDL);
			    shared_network_dereference(&nc->next, MDL);
			}

			/* As are pools. */
			if (nc->pools) 
			{
			    pool_reference(&pn, nc->pools, MDL);
			    do {
					LEASE_STRUCT_PTR lptr[RESERVED_LEASES + 1];

					if (pn) 
					{
					    pool_reference(&pc, pn, MDL);
					    pool_dereference(&pn, MDL);
					}
					
					if (pc->next) 
					{
					    pool_reference(&pn, pc->next, MDL);
					    pool_dereference(&pc->next, MDL);
					}

					lptr[FREE_LEASES] 	   = &pc->free;
					lptr[ACTIVE_LEASES]    = &pc->active;
					lptr[EXPIRED_LEASES]   = &pc->expired;
					lptr[ABANDONED_LEASES] = &pc->abandoned;
					lptr[BACKUP_LEASES]    = &pc->backup;
					lptr[RESERVED_LEASES]  = &pc->reserved;

					/* As (sigh) are leases. */
					for (i = FREE_LEASES ; i <= RESERVED_LEASES ; i++) 
					{
						if (LEASE_NOT_EMPTYP(lptr[i])) 
						{
					        lease_reference(&ln, LEASE_GET_FIRSTP(lptr[i]), MDL);
							do {
							    /* save a pointer to the current lease */
							    lease_reference(&lc, ln, MDL);
							    lease_dereference(&ln, MDL);

							    /* get the next lease if there is one */
							    ltemp = LEASE_GET_NEXTP(lptr[i], lc);
							    if (ltemp != NULL) 
								{
									lease_reference(&ln, ltemp, MDL);
							    }

							    /* remove the current lease from the queue */
							    LEASE_REMOVEP(lptr[i], lc);

							    if (lc->billing_class)
							       class_dereference(&lc->billing_class,
										  MDL);
							    if (lc->state)
								free_lease_state(lc->state, MDL);
							    lc->state = (struct lease_state *)0;
							    if (lc->n_hw)
									lease_dereference(&lc->n_hw, MDL);
							    if (lc->n_uid)
									lease_dereference(&lc->n_uid, MDL);
								
							    lease_dereference (&lc, MDL);
							} while (ln);
					    }
					}
					if (pc->group)
					    group_dereference(&pc->group, MDL);
					if (pc->shared_network)
					    shared_network_dereference(&pc->shared_network, MDL);
					pool_dereference(&pc, MDL);
			    } while (pn);
			    pool_dereference(&nc->pools, MDL);
			}
			/* Because of a circular reference, we need to nuke this
			   manually. */
			group_dereference(&nc->group, MDL);
			shared_network_dereference(&nc, MDL);
	    } while (nn);
	    shared_network_dereference(&shared_networks, MDL);
	}

	cancel_all_timeouts();
	relinquish_timeouts();
#if defined(DELAYED_ACK)
	relinquish_ackqueue();
#endif
	trace_free_all();
	group_dereference(&root_group, MDL);
	executable_statement_dereference(&default_classification_rules, MDL);

	shutdown_state = shutdown_drop_omapi_connections;
	omapi_io_state_foreach(dhcp_io_shutdown, 0);
	shutdown_state = shutdown_listeners;
	omapi_io_state_foreach(dhcp_io_shutdown, 0);
	shutdown_state = shutdown_dhcp;
	omapi_io_state_foreach(dhcp_io_shutdown, 0);

	omapi_object_dereference((omapi_object_t **)&icmp_state, MDL);

	universe_free_hash_table (&universe_hash, MDL);
	for (i = 0; i < universe_count; i++) 
	{
#if 0
		union {
			const char *c;
			char *s;
		} foo;
#endif
		if (universes [i]) 
		{
			if (universes[i]->name_hash)
			    option_name_free_hash_table(&universes[i]->name_hash, MDL);
			if (universes[i]->code_hash)
			    option_code_free_hash_table(&universes[i]->code_hash, MDL);
#if 0
			if (universes [i] -> name > (char *)&end) {
				foo.c = universes [i] -> name;
				dfree (foo.s, MDL);
			}
			if (universes [i] > (struct universe *)&end)
				dfree (universes [i], MDL);
#endif
		}
	}
	dfree(universes, MDL);

	relinquish_free_lease_states();
	relinquish_free_pairs();
	relinquish_free_expressions();
	relinquish_free_binding_values();
	relinquish_free_option_caches();
	relinquish_free_packets();
#if defined(COMPACT_LEASES)
	relinquish_lease_hunks();
#endif
	relinquish_hash_bucket_hunks();
	omapi_type_relinquish();
}
#endif /* DEBUG_MEMORY_LEAKAGE_ON_EXIT */
