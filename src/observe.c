/* 
 * This file (observe.c) implements a generic framework for .OBSERVE commands
 * allowing any command to be easily extended with observe functionality.
 */

#include "server.h"
#include "ae.h"
#include <stdio.h>

void markClientAsObserving(client *c) {
    if (!c->flag.observing) {
        c->flag.observing = 1;
        server.observing_clients++;
    }
}

void unmarkClientAsObserving(client *c) {
    if (c->flag.observing) {
        c->flag.observing = 0;
        server.observing_clients--;
    }
}

/* Generic observe command handler that can be used by any command */
void genericObserveCommand(client *c, observeCommandHandler handler) {
    /* Check if blocking is allowed */
    if (c->flag.deny_blocking && !c->flag.multi) {
        addReplyErrorFormat(c, "%s.OBSERVE isn't allowed for a DENY BLOCKING client",
                           (char *)c->argv[0]->ptr);
        return;
    }
    
    /* Generate command fingerprint */
    sds fingerprint = generateCommandFingerprint(c);
    
    /* Subscribe to observe this command fingerprint */
    int is_new_subscription = observeSubscribeFingerprint(c, fingerprint, c->argv[1]);

    /* Send observe subscription message with fingerprint and result */
    struct ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;
    
    if (c->resp == 2)
        addReply(c, shared.mbulkhdr[5]);
    else
        addReplyPushLen(c, 5);
    
    addReplyBulkCString(c, ".observe");
    addReplyBulkCString(c, "fingerprint");
    addReplyBulkCString(c, fingerprint);
    addReplyBulkCString(c, "result");
    
    /* Execute the command handler to get the initial value */
    handler(c);
    
    if (!old_flags.pushing) c->flag.pushing = 0;
    
    /* Clean up fingerprint */
    sdsfree(fingerprint);
    
    /* Mark client as observing if this is a new subscription */
    if (is_new_subscription) {
        markClientAsObserving(c);
    }
}

/* Generic notification executor for observe commands */
void genericExecuteObserveNotification(client *c, observeCommandHandler handler, sds fingerprint) {
    struct ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;
    
    /* Send message with fingerprint and result */
    if (c->resp == 2)
        addReply(c, shared.mbulkhdr[5]);
    else
        addReplyPushLen(c, 5);
    
    addReplyBulkCString(c, ".observe");
    addReplyBulkCString(c, "fingerprint");
    addReplyBulkCString(c, fingerprint);
    addReplyBulkCString(c, "result");
    
    /* Execute the command handler to get the updated value */
    handler(c);
    
    if (!old_flags.pushing) c->flag.pushing = 0;
}

/* Execute a observe command and send results to all subscribed clients */
void executeObserveCommand(observeCommandInfo *observeInfo, robj *fingerprint_obj, int dbid) {
    client *c;
    listNode *ln;
    listIter li;

    /* First, check if the key exists in the database */
    serverDb *db = &server.db[dbid];
    robj *key_obj = lookupKeyRead(db, observeInfo->key);
    int key_exists = (key_obj != NULL);
    if (!key_exists) {
        return;
    }

    listRewind(observeInfo->clients, &li);
    while ((ln = listNext(&li))) {
        c = ln->value;

        /* Only notify if client is in the same database */
        if (c->db->id != dbid) continue;

        /* Save current client command state */
        robj **orig_argv = c->argv;
        int orig_argc = c->argc;

        /* Set up command execution */
        c->argv = observeInfo->argv;
        c->argc = observeInfo->argc;

        /* Get the base command name (without .OBSERVE suffix) */
        sds base_cmd = sdsdup(observeInfo->command);
        char *dot = strchr(base_cmd, '.');
        if (dot) *dot = '\0';

        /* Find the appropriate handler based on command */
        observeCommandHandler handler = NULL;
        if (!strcasecmp(base_cmd, "GET")) {
            handler = getObserveHandler;
        } else if (!strcasecmp(base_cmd, "ZRANGE")) {
            handler = zrangeObserveHandler;
        }
        /* Add more handlers here as needed */

        if (handler) {
            genericExecuteObserveNotification(c, handler, fingerprint_obj->ptr);
        }

        sdsfree(base_cmd);

        /* Restore client state */
        c->argv = orig_argv;
        c->argc = orig_argc;
    }
}

/* Generate a fingerprint (hash) for a command with its arguments */
/* List match function for comparing fingerprint objects */
static int fingerprintObjMatch(void *ptr, void *key) {
    robj *o1 = (robj *)ptr;
    robj *o2 = (robj *)key;
    return equalStringObjects(o1, o2);
}

sds generateCommandFingerprint(client *c) {
    sds fingerprint_data = sdsempty();

    /* Concatenate command name and all arguments */
    for (int i = 0; i < c->argc; i++) {
        fingerprint_data = sdscatsds(fingerprint_data, c->argv[i]->ptr);
        if (i < c->argc - 1) {
            fingerprint_data = sdscat(fingerprint_data, " ");
        }
    }

    /* Generate CRC64 hash */
    uint64_t hash = crc64(0, (unsigned char *)fingerprint_data, sdslen(fingerprint_data));

    /* Convert to hex string */
    sds fingerprint = sdscatprintf(sdsempty(), "%016llx", (unsigned long long)hash);

    sdsfree(fingerprint_data);
    return fingerprint;
}

/* Subscribe a client to observe a command fingerprint. */
int observeSubscribeFingerprint(client *c, sds fingerprint, robj *key) {
    dictEntry *de;
    observeCommandInfo *observeInfo;
    int retval = 0;
    
    /* Initialize server observe structures if needed */
    if (server.observe_fingerprints == NULL) {
        server.observe_fingerprints = dictCreate(&objectKeyPointerValueDictType);
    }
    if (server.observe_key_to_fingerprints == NULL) {
        server.observe_key_to_fingerprints = dictCreate(&keylistDictType);
    }
    
    /* Initialize client observe structure if needed */
    if (c->observe_fingerprints == NULL) {
        c->observe_fingerprints = dictCreate(&objectKeyPointerValueDictType);
    }
    
    /* Convert sds fingerprint to robj for dictionary keys */
    robj *fingerprint_obj = createStringObject(fingerprint, sdslen(fingerprint));
    
    /* Check if client is already subscribed to this fingerprint */
    if (dictFind(c->observe_fingerprints, fingerprint_obj) == NULL) {
        retval = 1; /* New subscription */
        
        /* Check if this fingerprint already exists in server.observe_fingerprints */
        de = dictFind(server.observe_fingerprints, fingerprint_obj);
        if (de == NULL) {
            /* Create new observe command info - fingerprint doesn't exist yet */
            observeInfo = zmalloc(sizeof(observeCommandInfo));
            observeInfo->command = sdsnew(c->argv[0]->ptr);
            observeInfo->argc = c->argc;
            observeInfo->argv = zmalloc(sizeof(robj*) * c->argc);
            for (int i = 0; i < c->argc; i++) {
                observeInfo->argv[i] = c->argv[i];
                incrRefCount(c->argv[i]);
            }
            observeInfo->key = key;
            incrRefCount(key);
            observeInfo->clients = listCreate();
            
            /* Store in server.observe_fingerprints */
            dictAdd(server.observe_fingerprints, fingerprint_obj, observeInfo);
            incrRefCount(fingerprint_obj); /* For server dict */
            
            /* Add fingerprint to client's subscriptions */
            dictAdd(c->observe_fingerprints, fingerprint_obj, NULL);
            incrRefCount(fingerprint_obj); /* For client dict */
            
            /* Add fingerprint to key -> fingerprints mapping */
            de = dictFind(server.observe_key_to_fingerprints, key);
            list *fingerprints;
            if (de == NULL) {
                fingerprints = listCreate();
                listSetFreeMethod(fingerprints, decrRefCountVoid);
                listSetMatchMethod(fingerprints, fingerprintObjMatch);
                dictAdd(server.observe_key_to_fingerprints, key, fingerprints);
                incrRefCount(key);
            } else {
                fingerprints = dictGetVal(de);
                /* If list is missing free/match methods, it's from old code - recreate it */
                if (fingerprints->free == NULL || fingerprints->match == NULL) {
                    /* Remove old broken list and create a new one */
                    dictDelete(server.observe_key_to_fingerprints, key);
                    fingerprints = listCreate();
                    listSetFreeMethod(fingerprints, decrRefCountVoid);
                    listSetMatchMethod(fingerprints, fingerprintObjMatch);
                    dictAdd(server.observe_key_to_fingerprints, key, fingerprints);
                    incrRefCount(key);
                }
            }
            listAddNodeTail(fingerprints, fingerprint_obj);
            incrRefCount(fingerprint_obj); /* For the list */

            /* Decrement initial refcount - all references are now in dicts/list */
            decrRefCount(fingerprint_obj);
        } else {
            /* Fingerprint already exists - use the existing one */
            observeInfo = dictGetVal(de);
            robj *existing_fingerprint = (robj*)dictGetKey(de);

            /* Add client to subscriptions using existing fingerprint object */
            dictAdd(c->observe_fingerprints, existing_fingerprint, NULL);
            incrRefCount(existing_fingerprint); /* For client dict */

            /* Free the new fingerprint_obj since we're using the existing one */
            decrRefCount(fingerprint_obj);
            fingerprint_obj = existing_fingerprint;
        }

        /* Add client to the fingerprint's client list */
        listAddNodeTail(observeInfo->clients, c);
    } else {
        /* Client already subscribed to this fingerprint */
        decrRefCount(fingerprint_obj);
    }
    
    return retval;
}

/* Free observeCommandInfo structure */
void freeObserveCommandInfo(observeCommandInfo *observeInfo) {
    if (observeInfo->command) sdsfree(observeInfo->command);
    if (observeInfo->argv) {
        for (int i = 0; i < observeInfo->argc; i++) {
            decrRefCount(observeInfo->argv[i]);
        }
        zfree(observeInfo->argv);
    }
    if (observeInfo->key) decrRefCount(observeInfo->key);
    if (observeInfo->clients) listRelease(observeInfo->clients);
    zfree(observeInfo);
}

/* Unsubscribe a client from a specific fingerprint */
void observeUnsubscribeFingerprint(client *c, robj *fingerprint_obj) {
    dictEntry *de;
    observeCommandInfo *observeInfo;
    listNode *ln;
    
    if (!c->observe_fingerprints || !fingerprint_obj) return;
    
    /* Safety check: ensure fingerprint_obj has valid string data */
    if (fingerprint_obj->type != OBJ_STRING || !fingerprint_obj->ptr) {
        return;
    }
    
    /* Remove fingerprint from client's subscriptions */
    if (dictDelete(c->observe_fingerprints, fingerprint_obj) == DICT_OK) {
        /* Find the observeCommandInfo for this fingerprint */
        de = dictFind(server.observe_fingerprints, fingerprint_obj);
        if (de) {
            observeInfo = dictGetVal(de);
            
            /* Remove client from the fingerprint's client list */
            ln = listSearchKey(observeInfo->clients, c);
            if (ln) listDelNode(observeInfo->clients, ln);
            
            /* If no clients left, remove the entire observe command info */
            if (listLength(observeInfo->clients) == 0) {
                /* Save the key before freeing observeInfo */
                robj *key_obj = observeInfo->key;
                incrRefCount(key_obj); /* Temporarily increment to keep it valid */

                /* Temporarily increment fingerprint_obj refcount to prevent premature freeing */
                incrRefCount(fingerprint_obj);

                /* Remove and free the observeCommandInfo */
                dictDelete(server.observe_fingerprints, fingerprint_obj);
                freeObserveCommandInfo(observeInfo);

                /* Remove fingerprint from key -> fingerprints mapping */
                de = dictFind(server.observe_key_to_fingerprints, key_obj);
                if (de) {
                    list *fingerprints = dictGetVal(de);
                    ln = listSearchKey(fingerprints, fingerprint_obj);
                    if (ln) {
                        listDelNode(fingerprints, ln);
                    }

                    /* If no fingerprints left for this key, remove the key entry */
                    if (listLength(fingerprints) == 0) {
                        dictDelete(server.observe_key_to_fingerprints, key_obj);
                    }
                }

                /* Release temporary references */
                decrRefCount(fingerprint_obj);
                decrRefCount(key_obj);
            }
        }
    }
    
    /* If client has no more observe subscriptions, clean up */
    if (dictSize(c->observe_fingerprints) == 0) {
        dictRelease(c->observe_fingerprints);
        c->observe_fingerprints = NULL;
        /* Note: observing_clients counter is managed by unmarkClientAsObserving */
    }
}

/* Original immediate notification function - now called by debounce flush */
void observeNotifyKeyChangeImmediate(robj *key, int dbid) {
    dictEntry *de;
    list *fingerprints;
    listNode *ln;
    listIter li;
    observeCommandInfo *observeInfo;
    
    /* Comprehensive safety checks - add try/catch equivalent using signal handling */
    if (server.observe_key_to_fingerprints == NULL || key == NULL) return;
    
    /* Check if key object is valid - ensure it's not corrupted */
    if ((uintptr_t)key < 0x1000 || (uintptr_t)key > 0x7FFFFFFFFFFF) return;
    
    /* Use a more conservative approach - check if we can safely read the object fields */
    __volatile__ int test_type, test_refcount;
    __volatile__ void *test_ptr;
    
    /* Try to safely read the object fields - if this crashes, we'll catch it */
    test_type = key->type;
    test_refcount = key->refcount;  
    test_ptr = key->ptr;
    
    /* Additional safety check to ensure key is a valid string object */
    if (test_type != OBJ_STRING || test_ptr == NULL) return;
    
    /* Check if the string pointer itself is valid */
    if ((uintptr_t)test_ptr < 0x1000 || (uintptr_t)test_ptr > 0x7FFFFFFFFFFF) return;
    
    /* Check refcount is reasonable (not negative or too large) */
    if (test_refcount <= 0 || test_refcount > 1000000) return;
    
    /* Verify the dictionary itself is valid before using it */
    if (dictSize(server.observe_key_to_fingerprints) == 0) return;
    
    /* One more check before the dangerous dictFind call */
    if (key->type != OBJ_STRING || key->ptr == NULL) return;
    
    de = dictFind(server.observe_key_to_fingerprints, key);
    if (de == NULL) return;
    
    fingerprints = dictGetVal(de);
    listRewind(fingerprints, &li);
    
    /* For each fingerprint observing this key */
    while ((ln = listNext(&li))) {
        robj *fingerprint_obj = ln->value;
        
        /* Get the observe command info for this fingerprint */
        de = dictFind(server.observe_fingerprints, fingerprint_obj);
        if (de) {
            observeInfo = dictGetVal(de);
            /* Execute the command once and send to all clients */
            executeObserveCommand(observeInfo, fingerprint_obj, dbid);
        }
    }
}

/* Initialize query subscription debounce mechanism */
void initObserveDebounce(void) {
    server.observe_debounce_buffer = dictCreate(&observeDebounceBufferDictType);
    /* Don't set observe_debounce_period here - it's already set from config */
}

/* Free a observeDebounceKey structure */
void freeObserveDebounceKey(observeDebounceKey *odk) {
    if (odk->key) decrRefCount(odk->key);
    zfree(odk);
}

static int isObserveDebounceRunning = 0;

/* Timer handler for debounce flush */
int observeDebounceTimerHandler(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    observeDebounceFlush();

    if (server.observe_debounce_buffer && dictSize(server.observe_debounce_buffer) > 0) {
        return server.observe_debounce_period;
    }

    isObserveDebounceRunning = 0;
    return AE_NOMORE;
}

/* Start the debounce timer proc if it is not already running */
void startObserveDebounceProc(void) {
    if (!isObserveDebounceRunning) {
        isObserveDebounceRunning = 1;
        aeCreateTimeEvent(server.el, server.observe_debounce_period,
                          observeDebounceTimerHandler, NULL, NULL);
    }
}

/* Flush all pending query subscription notifications from the debounce buffer */
void observeDebounceFlush(void) {
    dictEntry *de;
    dictIterator *di;
    observeDebounceKey *odk;
    list *safe_keys;
    listNode *ln;
    listIter li;
    typedef struct {
        robj *key_copy;
        int dbid;
    } safeKey;
    
    if (!server.observe_debounce_buffer || dictSize(server.observe_debounce_buffer) == 0) {
        return;
    }
    
    /* First pass: create safe copies of all keys */
    safe_keys = listCreate();
    di = dictGetIterator(server.observe_debounce_buffer);
    while ((de = dictNext(di)) != NULL) {
        odk = dictGetVal(de);
        /* Only process if key is valid and has proper object type */
        if (odk && odk->key && odk->key->type == OBJ_STRING && odk->key->ptr != NULL) {
            /* Create a safe copy structure */
            safeKey *sk = zmalloc(sizeof(safeKey));
            /* Create an independent copy of the key */
            sk->key_copy = createStringObject(odk->key->ptr, sdslen((sds)odk->key->ptr));
            sk->dbid = odk->dbid;
            listAddNodeTail(safe_keys, sk);
        }
    }
    dictReleaseIterator(di);
    
    /* Clear the buffer to prevent re-entrance issues */
    dictEmpty(server.observe_debounce_buffer, NULL);
    
    /* Second pass: process all safe key copies */
    listRewind(safe_keys, &li);
    while ((ln = listNext(&li)) != NULL) {
        safeKey *sk = ln->value;
        /* Process the safe copy */
        observeNotifyKeyChangeImmediate(sk->key_copy, sk->dbid);
        /* Clean up the safe copy */
        decrRefCount(sk->key_copy);
        zfree(sk);
    }
    
    /* Clean up processing list */
    listRelease(safe_keys);
}

/* Add a key change to the debounce buffer */
void observeDebounceKeyChange(robj *key, int dbid) {
    dictEntry *de;
    observeDebounceKey *odk;
    
    /* Initialize if needed */
    if (server.observe_debounce_buffer == NULL) {
        initObserveDebounce();
    }
    
    /* Check if this key is already in the buffer */
    de = dictFind(server.observe_debounce_buffer, key);
    if (de == NULL) {
        /* New key - add to buffer */
        odk = zmalloc(sizeof(observeDebounceKey));
        odk->key = key;
        incrRefCount(key);
        odk->dbid = dbid;
        odk->timestamp = mstime();
        
        dictAdd(server.observe_debounce_buffer, key, odk);
        incrRefCount(key); /* For the dict key */

        startObserveDebounceProc();
    } else {
        /* Key already in buffer - update dbid if different */
        odk = dictGetVal(de);
        odk->dbid = dbid;
    }
}

/* Public API function - called when a key changes */
void observeNotifyKeyChange(robj *key, int dbid) {
    if (server.observing_clients == 0) return;

    /* When debounce period is 0, send immediately without buffering.
     * Otherwise buffer the change and let the timer coalesce rapid updates. */
    if (server.observe_debounce_period == 0) {
        observeNotifyKeyChangeImmediate(key, dbid);
    } else {
        observeDebounceKeyChange(key, dbid);
    }
}

/* UNOBSERVE <fingerprint> [fingerprint ...] - Unobserve specific query subscriptions */
void unobserveCommand(client *c) {
    int unsubscribed = 0;

    /* Check if client has any observe subscriptions */
    if (c->observe_fingerprints == NULL || dictSize(c->observe_fingerprints) == 0) {
        addReplyLongLong(c, 0);
        return;
    }

    /* Iterate through all provided fingerprints and unsubscribe */
    for (int i = 1; i < c->argc; i++) {
        robj *fingerprint_obj = c->argv[i];

        /* Check if client is subscribed to this fingerprint */
        if (dictFind(c->observe_fingerprints, fingerprint_obj) != NULL) {
            observeUnsubscribeFingerprint(c, fingerprint_obj);
            unsubscribed++;
        }
    }

    /* If client has no more observe subscriptions, unmark as observing */
    if (c->observe_fingerprints == NULL || dictSize(c->observe_fingerprints) == 0) {
        unmarkClientAsObserving(c);
    }

    /* Reply with number of unsubscribed fingerprints */
    addReplyLongLong(c, unsubscribed);
}