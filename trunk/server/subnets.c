/*
 * file:	subnets.c
 * purpose:	to retrieve the subnet information and convert it to a binary form
 * created:	03/23/2007
 * creator: rian shelley
 */

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include "subnets.h"
#include <string.h>
#include <unistd.h>
#include "sockutils.h"
#include "base64.h"
#include <curl/curl.h>

CURL* curlhandle=NULL;

typedef struct _sinfo {
    subnet* subnets;
    int max;
    int size;
} sinfo;

size_t write_data(void* buffer, size_t size, size_t nmemb, void* userp)
{
    sinfo* s = (sinfo*)userp;
    unsigned int cur = 0;
    while ( cur < size * nmemb )
    {
        memcpy(&(s->subnets[s->size++]), buffer + cur, SUBSTRUCT_SIZE);
        cur += SUBSTRUCT_SIZE;
        if (s->size >= s->max)
            break;
    }
    return cur;
}

void cleanup()
{
    if (curlhandle)
        curl_easy_cleanup(curlhandle);
}


/*
 * function:    getsubnets()
 * purpose:     to retrieve a list of subnets from a web page
 * recieves:    a pointer to an array of subnets, the size of the array,
 *              the server to get the list from, the path to the page,
 *              and the authorization required to log into the website as a
 *              username:password mime-encoded string. 
 */
int getsubnets(subnet subnets[], int max, const char* webpage, const char* authentication)
{
    if (curlhandle == 0)
    {
        if (!(curlhandle = curl_easy_init()))
            return 0;
        atexit(cleanup);
        curl_easy_setopt(curlhandle, CURLOPT_SSL_VERIFYHOST, NULL);
    }
    sinfo subnetlist;
    subnetlist.subnets = subnets;
    subnetlist.max = max;
    subnetlist.size = 0;
    curl_easy_setopt(curlhandle, CURLOPT_URL, webpage);
    curl_easy_setopt(curlhandle, CURLOPT_USERPWD, authentication);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curlhandle, CURLOPT_WRITEDATA, &subnetlist);

    //and do it...
    curl_easy_perform(curlhandle);

    return subnetlist.size;
}


