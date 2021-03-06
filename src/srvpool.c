// Copyright 2015 Apcera Inc. All rights reserved.

#include "natsp.h"

#include "mem.h"
#include "url.h"

static void
_freeSrv(natsSrv *srv)
{
    if (srv == NULL)
        return;

    natsUrl_Destroy(srv->url);
    NATS_FREE(srv);
}

static natsStatus
_createSrv(natsSrv **newSrv, char *url)
{
    natsStatus  s = NATS_OK;
    natsSrv     *srv = (natsSrv*) NATS_CALLOC(1, sizeof(natsSrv));

    if (srv == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    s = natsUrl_Create(&(srv->url), url);
    if (s == NATS_OK)
        *newSrv = srv;
    else
        _freeSrv(srv);

    return NATS_UPDATE_ERR_STACK(s);
}

natsSrv*
natsSrvPool_GetCurrentServer(natsSrvPool *pool, const natsUrl *url, int *index)
{
    natsSrv *s = NULL;
    int     i;

    for (i = 0; i < pool->size; i++)
    {
        s = pool->srvrs[i];
        if (s->url == url)
        {
            if (index != NULL)
                *index = i;

            return s;
        }
    }

    if (index != NULL)
        *index = -1;

    return NULL;
}

// Pop the current server and put onto the end of the list. Select head of list as long
// as number of reconnect attempts under MaxReconnect.
natsSrv*
natsSrvPool_GetNextServer(natsSrvPool *pool, natsOptions *opts, const natsUrl *ncUrl)
{
    natsSrv *s = NULL;
    int     i, j;

    s = natsSrvPool_GetCurrentServer(pool, ncUrl, &i);
    if (i < 0)
        return NULL;

    // Shift left servers past current to the current's position
    for (j = i; j < pool->size - 1; j++)
        pool->srvrs[j] = pool->srvrs[j+1];

    if ((opts->maxReconnect < 0)
        || (s->reconnects < opts->maxReconnect))
    {
        // Move the current server to the back of the list
        pool->srvrs[pool->size - 1] = s;
    }
    else
    {
        // Remove the server from the list
        _freeSrv(s);
        pool->size--;
    }

    if (pool->size <= 0)
        return NULL;

    return pool->srvrs[0];
}

void
natsSrvPool_Destroy(natsSrvPool *pool)
{
    natsSrv *srv;
    int     i;

    if (pool == NULL)
        return;

    for (i = 0; i < pool->size; i++)
    {
        srv = pool->srvrs[i];
        _freeSrv(srv);
    }
    natsStrHash_Destroy(pool->urls);
    pool->urls = NULL;

    NATS_FREE(pool->srvrs);
    pool->srvrs = NULL;
    pool->size  = 0;
    NATS_FREE(pool);
}

static natsStatus
_addURLToPool(natsSrvPool *pool, char *sURL)
{
    natsStatus  s;
    natsSrv     *srv = NULL;
    bool        addedToMap = false;
    char        bareURL[256];

    s = _createSrv(&srv, sURL);
    if (s != NATS_OK)
        return s;

    // In the map, we need to add an URL that is just host:port
    snprintf(bareURL, sizeof(bareURL), "%s:%d", srv->url->host, srv->url->port);
    s = natsStrHash_Set(pool->urls, bareURL, true, (void*)1, NULL);
    if (s == NATS_OK)
    {
        addedToMap = true;
        if (pool->size + 1 > pool->cap)
        {
            natsSrv **newArray  = NULL;
            int     newCap      = 2 * pool->cap;

            newArray = (natsSrv**) NATS_REALLOC(pool->srvrs, newCap * sizeof(char*));
            if (newArray == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);

            if (s == NATS_OK)
            {
                pool->cap = newCap;
                pool->srvrs = newArray;
            }
        }
        if (s == NATS_OK)
            pool->srvrs[pool->size++] = srv;
    }
    if (s != NATS_OK)
    {
        if (addedToMap)
            natsStrHash_Remove(pool->urls, sURL);

        _freeSrv(srv);
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static void
_shufflePool(natsSrvPool *pool)
{
    int     i, j;
    natsSrv *tmp;

    if (pool->size <= 1)
        return;

    srand((unsigned int) nats_NowInNanoSeconds());

    for (i = 0; i < pool->size; i++)
    {
        j = rand() % (i + 1);
        tmp = pool->srvrs[i];
        pool->srvrs[i] = pool->srvrs[j];
        pool->srvrs[j] = tmp;
    }
}

natsStatus
natsSrvPool_addNewURLs(natsSrvPool *pool, char **urls, int urlCount, bool doShuffle)
{
    natsStatus  s       = NATS_OK;
    bool        updated = false;
    char        url[256];
    int         i;

    for (i=0; (s == NATS_OK) && (i<urlCount); i++)
    {
        if (natsStrHash_Get(pool->urls, urls[i]) == NULL)
        {
            snprintf(url, sizeof(url), "nats://%s", urls[i]);
            s = _addURLToPool(pool, url);
            if (s == NATS_OK)
                updated = true;
        }
    }
    if (updated && doShuffle)
        _shufflePool(pool);

    return NATS_UPDATE_ERR_STACK(s);
}

// Create the server pool using the options given.
// We will place a Url option first, followed by any
// Server Options. We will randomize the server pool unlesss
// the NoRandomize flag is set.
natsStatus
natsSrvPool_Create(natsSrvPool **newPool, natsOptions *opts)
{
    natsStatus  s        = NATS_OK;
    natsSrvPool *pool    = NULL;
    int         poolSize;
    int         i;

    poolSize  = (opts->url != NULL ? 1 : 0);
    poolSize += opts->serversCount;

    // If the pool is going to be empty, we will add the default URL.
    if (poolSize == 0)
        poolSize = 1;

    pool = (natsSrvPool*) NATS_CALLOC(1, sizeof(natsSrvPool));
    if (pool == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    pool->srvrs = (natsSrv**) NATS_CALLOC(poolSize, sizeof(natsSrv*));
    if (pool->srvrs == NULL)
    {
        NATS_FREE(pool);
        return nats_setDefaultError(NATS_NO_MEMORY);
    }
    // Set the current capacity. The array of urls may have to grow in
    // the future.
    pool->cap = poolSize;

    // Map that helps find out if an URL is already known.
    s = natsStrHash_Create(&(pool->urls), poolSize);

    if ((s == NATS_OK) && (opts->url != NULL))
        s = _addURLToPool(pool, opts->url);

    // Add URLs from Options' Servers
    for (i=0; (s == NATS_OK) && (i < opts->serversCount); i++)
        s = _addURLToPool(pool, opts->servers[i]);

    if (s == NATS_OK)
    {
        // Randomize if allowed to
        if (!(opts->noRandomize))
            _shufflePool(pool);

        if (pool->size == 0)
        {
            // Place default URL if pool is empty.
            s = _addURLToPool(pool, (char*) NATS_DEFAULT_URL);
        }
    }

    if (s == NATS_OK)
        *newPool = pool;
    else
        natsSrvPool_Destroy(pool);

    return NATS_UPDATE_ERR_STACK(s);
}
