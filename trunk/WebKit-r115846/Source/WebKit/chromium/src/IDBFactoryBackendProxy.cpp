/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IDBFactoryBackendProxy.h"

#if ENABLE(INDEXED_DATABASE)

#include "CrossThreadTask.h"
#include "DOMStringList.h"
#include "IDBDatabaseBackendProxy.h"
#include "IDBDatabaseError.h"
#include "SecurityOrigin.h"
#include "WebFrameImpl.h"
#include "WebIDBCallbacksImpl.h"
#include "WebIDBDatabase.h"
#include "WebIDBDatabaseError.h"
#include "WebIDBFactory.h"
#include "WebKit.h"
#include "platform/WebKitPlatformSupport.h"
#include "WebPermissionClient.h"
#include "platform/WebVector.h"
#include "WebViewImpl.h"
#include "WebWorkerBase.h"
#include "WebWorkerClientImpl.h"
#include "WorkerContext.h"
#include "WorkerLoaderProxy.h"
#include "WorkerScriptController.h"
#include "WorkerThread.h"


using namespace WebCore;

namespace WebKit {

PassRefPtr<IDBFactoryBackendInterface> IDBFactoryBackendProxy::create()
{
    return adoptRef(new IDBFactoryBackendProxy());
}

IDBFactoryBackendProxy::IDBFactoryBackendProxy()
    : m_webIDBFactory(webKitPlatformSupport()->idbFactory())
{
}

IDBFactoryBackendProxy::~IDBFactoryBackendProxy()
{
}

void IDBFactoryBackendProxy::getDatabaseNames(PassRefPtr<IDBCallbacks> callbacks, PassRefPtr<SecurityOrigin> prpOrigin, Frame* frame, const String& dataDir)
{
    WebSecurityOrigin origin(prpOrigin);
    WebFrameImpl* webFrame = WebFrameImpl::fromFrame(frame);
    WebViewImpl* webView = webFrame->viewImpl();

    if (webView->permissionClient() && !webView->permissionClient()->allowIndexedDB(webFrame, "Database Listing", origin)) {
        callbacks->onError(WebIDBDatabaseError(0, "The user denied permission to access the database."));
        return;
    }

    m_webIDBFactory->getDatabaseNames(new WebIDBCallbacksImpl(callbacks), origin, webFrame, dataDir);
}

static const char allowIndexedDBMode[] = "allowIndexedDBMode";

class AllowIndexedDBMainThreadBridge : public ThreadSafeRefCounted<AllowIndexedDBMainThreadBridge> {
public:
    static PassRefPtr<AllowIndexedDBMainThreadBridge> create(WebWorkerBase* webWorkerBase, const String& mode, const String& name)
    {
        return adoptRef(new AllowIndexedDBMainThreadBridge(webWorkerBase, mode, name));
    }

    // These methods are invoked on the worker context.
    void cancel()
    {
        MutexLocker locker(m_mutex);
        m_webWorkerBase = 0;
    }

    bool result()
    {
        return m_result;
    }

    // This method is invoked on the main thread.
    void signalCompleted(bool result, const String& mode)
    {
        MutexLocker locker(m_mutex);
        if (m_webWorkerBase)
            m_webWorkerBase->postTaskForModeToWorkerContext(createCallbackTask(&didComplete, this, result), mode);
    }

private:
    AllowIndexedDBMainThreadBridge(WebWorkerBase* webWorkerBase, const String& mode, const String& name)
        : m_result(false)
        , m_webWorkerBase(webWorkerBase)
    {
        WebCommonWorkerClient* commonClient = webWorkerBase->commonClient();
        // See note about thread-safety below.
        WebWorkerBase::dispatchTaskToMainThread(
            createCallbackTask(&allowIndexedDBTask, this, WebCore::AllowCrossThreadAccess(commonClient), name, mode));
    }

    static void allowIndexedDBTask(ScriptExecutionContext*, PassRefPtr<AllowIndexedDBMainThreadBridge> bridge, WebCommonWorkerClient* commonClient, const String& name, const String& mode)
    {
        if (!commonClient) {
            bridge->signalCompleted(false, mode);
            return;
        }
        bool allowed = commonClient->allowIndexedDB(name);
        bridge->signalCompleted(allowed, mode);
    }

    static void didComplete(ScriptExecutionContext* context, PassRefPtr<AllowIndexedDBMainThreadBridge> bridge, bool result)
    {
        bridge->m_result = result;
    }

    bool m_result;
    Mutex m_mutex;
    // AllowIndexedDBMainThreadBridge uses two non-threadsafe classes across
    // threads: WebWorkerBase and WebCommonWorkerClient.
    // In the dedicated worker case, these are both the same object of type
    // WebWorkerClientImpl, which isn't deleted for the life of the renderer
    // process so we don't have to worry about use-after-frees.
    // In the shared worker case, these are of type WebSharedWorkerImpl and
    // chromium's WebSharedWorkerClientProxy, respectively. These are both
    // deleted on the main thread in response to a request on the worker thread,
    // but only after the worker run loop stops processing tasks. So even in
    // the most interleaved case, we have:
    // W AllowIndexedDBMainThreadBridge schedules allowIndexedDBTask
    // M workerRunLoop marked as killed
    // W runLoop stops and schedules object deletion on main thread
    // M allowIndexedDBTask calls commonClient->allowIndexedDB()
    // M WebWorkerBase and WebCommonWorkerClient are deleted
    WebWorkerBase* m_webWorkerBase;
};

bool IDBFactoryBackendProxy::allowIDBFromWorkerThread(WorkerContext* workerContext, const String& name, const WebSecurityOrigin&)
{

    WebWorkerBase* webWorkerBase = static_cast<WebWorkerBase*>(&workerContext->thread()->workerLoaderProxy());
    WorkerRunLoop& runLoop = workerContext->thread()->runLoop();

    String mode = allowIndexedDBMode;
    mode.append(String::number(runLoop.createUniqueId()));
    RefPtr<AllowIndexedDBMainThreadBridge> bridge = AllowIndexedDBMainThreadBridge::create(webWorkerBase, mode, name);

    // Either the bridge returns, or the queue gets terminated.
    if (runLoop.runInMode(workerContext, mode) == MessageQueueTerminated) {
        bridge->cancel();
        return false;
    }

    return bridge->result();
}

void IDBFactoryBackendProxy::openFromWorker(const String& name, IDBCallbacks* callbacks, PassRefPtr<SecurityOrigin> prpOrigin, WorkerContext* context, const String& dataDir)
{
#if ENABLE(WORKERS)
    WebSecurityOrigin origin(prpOrigin);
    if (!allowIDBFromWorkerThread(context, name, origin)) {
        callbacks->onError(WebIDBDatabaseError(0, "The user denied permission to access the database."));
        return;
    }
    m_webIDBFactory->open(name, new WebIDBCallbacksImpl(callbacks), origin, /*webFrame*/0, dataDir);
#endif
}

void IDBFactoryBackendProxy::open(const String& name, IDBCallbacks* callbacks, PassRefPtr<SecurityOrigin> prpOrigin, Frame* frame, const String& dataDir)
{
    WebSecurityOrigin origin(prpOrigin);
    WebFrameImpl* webFrame = WebFrameImpl::fromFrame(frame);
    WebViewImpl* webView = webFrame->viewImpl();
    if (webView->permissionClient() && !webView->permissionClient()->allowIndexedDB(webFrame, name, origin)) {
        callbacks->onError(WebIDBDatabaseError(0, "The user denied permission to access the database."));
        return;
    }

    m_webIDBFactory->open(name, new WebIDBCallbacksImpl(callbacks), origin, webFrame, dataDir);
}

void IDBFactoryBackendProxy::deleteDatabase(const String& name, PassRefPtr<IDBCallbacks> callbacks, PassRefPtr<SecurityOrigin> prpOrigin, Frame* frame, const String& dataDir)
{
    WebSecurityOrigin origin(prpOrigin);
    WebFrameImpl* webFrame = WebFrameImpl::fromFrame(frame);
    WebViewImpl* webView = webFrame->viewImpl();
    if (webView->permissionClient() && !webView->permissionClient()->allowIndexedDB(webFrame, name, origin)) {
        callbacks->onError(WebIDBDatabaseError(0, "The user denied permission to access the database."));
        return;
    }

    m_webIDBFactory->deleteDatabase(name, new WebIDBCallbacksImpl(callbacks), origin, webFrame, dataDir);
}

} // namespace WebKit

#endif // ENABLE(INDEXED_DATABASE)
