/*
 * Copyright (C) 2011, 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "core/inspector/InspectorFileSystemAgent.h"

#include "bindings/v8/ExceptionStatePlaceholder.h"
#include "core/dom/DOMImplementation.h"
#include "core/dom/Document.h"
#include "core/events/Event.h"
#include "core/fetch/TextResourceDecoder.h"
#include "core/fileapi/File.h"
#include "core/fileapi/FileError.h"
#include "core/fileapi/FileReader.h"
#include "core/html/VoidCallback.h"
#include "core/inspector/InspectorPageAgent.h"
#include "core/inspector/InspectorState.h"
#include "core/frame/Frame.h"
#include "core/platform/MIMETypeRegistry.h"
#include "modules/filesystem/DOMFileSystem.h"
#include "modules/filesystem/DirectoryEntry.h"
#include "modules/filesystem/DirectoryReader.h"
#include "modules/filesystem/EntriesCallback.h"
#include "modules/filesystem/Entry.h"
#include "modules/filesystem/EntryCallback.h"
#include "modules/filesystem/ErrorCallback.h"
#include "modules/filesystem/FileCallback.h"
#include "modules/filesystem/FileEntry.h"
#include "modules/filesystem/FileSystemCallbacks.h"
#include "modules/filesystem/LocalFileSystem.h"
#include "modules/filesystem/Metadata.h"
#include "modules/filesystem/MetadataCallback.h"
#include "weborigin/KURL.h"
#include "weborigin/SecurityOrigin.h"
#include "wtf/ArrayBuffer.h"
#include "wtf/text/Base64.h"
#include "wtf/text/TextEncoding.h"

using WebCore::TypeBuilder::Array;

typedef WebCore::InspectorBackendDispatcher::FileSystemCommandHandler::RequestFileSystemRootCallback RequestFileSystemRootCallback;
typedef WebCore::InspectorBackendDispatcher::FileSystemCommandHandler::RequestDirectoryContentCallback RequestDirectoryContentCallback;
typedef WebCore::InspectorBackendDispatcher::FileSystemCommandHandler::RequestMetadataCallback RequestMetadataCallback;
typedef WebCore::InspectorBackendDispatcher::FileSystemCommandHandler::RequestFileContentCallback RequestFileContentCallback;
typedef WebCore::InspectorBackendDispatcher::FileSystemCommandHandler::DeleteEntryCallback DeleteEntryCallback;

namespace WebCore {

namespace FileSystemAgentState {
static const char fileSystemAgentEnabled[] = "fileSystemAgentEnabled";
}

namespace {

template<typename BaseCallback, typename Handler, typename Argument>
class CallbackDispatcher : public BaseCallback {
public:
    typedef bool (Handler::*HandlingMethod)(Argument);

    static PassRefPtr<CallbackDispatcher> create(PassRefPtr<Handler> handler, HandlingMethod handlingMethod)
    {
        return adoptRef(new CallbackDispatcher(handler, handlingMethod));
    }

    virtual bool handleEvent(Argument argument) OVERRIDE
    {
        return (m_handler.get()->*m_handlingMethod)(argument);
    }

private:
    CallbackDispatcher(PassRefPtr<Handler> handler, HandlingMethod handlingMethod)
        : m_handler(handler)
        , m_handlingMethod(handlingMethod) { }

    RefPtr<Handler> m_handler;
    HandlingMethod m_handlingMethod;
};

template<typename BaseCallback>
class CallbackDispatcherFactory {
public:
    template<typename Handler, typename Argument>
    static PassRefPtr<CallbackDispatcher<BaseCallback, Handler, Argument> > create(Handler* handler, bool (Handler::*handlingMethod)(Argument))
    {
        return CallbackDispatcher<BaseCallback, Handler, Argument>::create(PassRefPtr<Handler>(handler), handlingMethod);
    }
};

class FileSystemRootRequest : public RefCounted<FileSystemRootRequest> {
    WTF_MAKE_NONCOPYABLE(FileSystemRootRequest);
public:
    static PassRefPtr<FileSystemRootRequest> create(PassRefPtr<RequestFileSystemRootCallback> requestCallback, const String& type)
    {
        return adoptRef(new FileSystemRootRequest(requestCallback, type));
    }

    void start(ExecutionContext*);

private:
    bool didHitError(FileError* error)
    {
        reportResult(error->code());
        return true;
    }

    bool didGetEntry(Entry*);

    void reportResult(FileError::ErrorCode errorCode, PassRefPtr<TypeBuilder::FileSystem::Entry> entry = 0)
    {
        m_requestCallback->sendSuccess(static_cast<int>(errorCode), entry);
    }

    FileSystemRootRequest(PassRefPtr<RequestFileSystemRootCallback> requestCallback, const String& type)
        : m_requestCallback(requestCallback)
        , m_type(type) { }

    RefPtr<RequestFileSystemRootCallback> m_requestCallback;
    String m_type;
};

void FileSystemRootRequest::start(ExecutionContext* executionContext)
{
    ASSERT(executionContext);

    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &FileSystemRootRequest::didHitError);

    FileSystemType type;
    if (!DOMFileSystemBase::pathPrefixToFileSystemType(m_type, type)) {
        errorCallback->handleEvent(FileError::create(FileError::SYNTAX_ERR).get());
        return;
    }

    KURL rootURL = DOMFileSystemBase::createFileSystemRootURL(executionContext->securityOrigin()->toString(), type);
    if (!rootURL.isValid()) {
        errorCallback->handleEvent(FileError::create(FileError::SYNTAX_ERR).get());
        return;
    }

    RefPtr<EntryCallback> successCallback = CallbackDispatcherFactory<EntryCallback>::create(this, &FileSystemRootRequest::didGetEntry);
    OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = ResolveURICallbacks::create(successCallback, errorCallback, executionContext);
    LocalFileSystem::from(executionContext)->resolveURL(executionContext, rootURL, fileSystemCallbacks.release());
}

bool FileSystemRootRequest::didGetEntry(Entry* entry)
{
    RefPtr<TypeBuilder::FileSystem::Entry> result = TypeBuilder::FileSystem::Entry::create()
        .setUrl(entry->toURL())
        .setName("/")
        .setIsDirectory(true);
    reportResult(static_cast<FileError::ErrorCode>(0), result);
    return true;
}

class DirectoryContentRequest : public RefCounted<DirectoryContentRequest> {
    WTF_MAKE_NONCOPYABLE(DirectoryContentRequest);
public:
    static PassRefPtr<DirectoryContentRequest> create(PassRefPtr<RequestDirectoryContentCallback> requestCallback, const String& url)
    {
        return adoptRef(new DirectoryContentRequest(requestCallback, url));
    }

    virtual ~DirectoryContentRequest()
    {
        reportResult(FileError::ABORT_ERR);
    }

    void start(ExecutionContext*);

private:
    bool didHitError(FileError* error)
    {
        reportResult(error->code());
        return true;
    }

    bool didGetEntry(Entry*);
    bool didReadDirectoryEntries(const EntryVector&);

    void reportResult(FileError::ErrorCode errorCode, PassRefPtr<Array<TypeBuilder::FileSystem::Entry> > entries = 0)
    {
        m_requestCallback->sendSuccess(static_cast<int>(errorCode), entries);
    }

    DirectoryContentRequest(PassRefPtr<RequestDirectoryContentCallback> requestCallback, const String& url)
        : m_requestCallback(requestCallback)
        , m_url(ParsedURLString, url) { }

    void readDirectoryEntries();

    RefPtr<RequestDirectoryContentCallback> m_requestCallback;
    KURL m_url;
    RefPtr<Array<TypeBuilder::FileSystem::Entry> > m_entries;
    RefPtr<DirectoryReader> m_directoryReader;
};

void DirectoryContentRequest::start(ExecutionContext* executionContext)
{
    ASSERT(executionContext);

    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &DirectoryContentRequest::didHitError);
    RefPtr<EntryCallback> successCallback = CallbackDispatcherFactory<EntryCallback>::create(this, &DirectoryContentRequest::didGetEntry);

    OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = ResolveURICallbacks::create(successCallback, errorCallback, executionContext);

    LocalFileSystem::from(executionContext)->resolveURL(executionContext, m_url, fileSystemCallbacks.release());
}

bool DirectoryContentRequest::didGetEntry(Entry* entry)
{
    if (!entry->isDirectory()) {
        reportResult(FileError::TYPE_MISMATCH_ERR);
        return true;
    }

    m_directoryReader = static_cast<DirectoryEntry*>(entry)->createReader();
    m_entries = Array<TypeBuilder::FileSystem::Entry>::create();
    readDirectoryEntries();
    return true;
}

void DirectoryContentRequest::readDirectoryEntries()
{
    if (!m_directoryReader->filesystem()->executionContext()) {
        reportResult(FileError::ABORT_ERR);
        return;
    }

    RefPtr<EntriesCallback> successCallback = CallbackDispatcherFactory<EntriesCallback>::create(this, &DirectoryContentRequest::didReadDirectoryEntries);
    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &DirectoryContentRequest::didHitError);
    m_directoryReader->readEntries(successCallback, errorCallback);
}

bool DirectoryContentRequest::didReadDirectoryEntries(const EntryVector& entries)
{
    if (entries.isEmpty()) {
        reportResult(static_cast<FileError::ErrorCode>(0), m_entries);
        return true;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        RefPtr<Entry> entry = entries[i];
        RefPtr<TypeBuilder::FileSystem::Entry> entryForFrontend = TypeBuilder::FileSystem::Entry::create()
            .setUrl(entry->toURL())
            .setName(entry->name())
            .setIsDirectory(entry->isDirectory());

        using TypeBuilder::Page::ResourceType;
        if (!entry->isDirectory()) {
            String mimeType = MIMETypeRegistry::getMIMETypeForPath(entry->name());
            ResourceType::Enum resourceType;
            if (MIMETypeRegistry::isSupportedImageMIMEType(mimeType)) {
                resourceType = ResourceType::Image;
                entryForFrontend->setIsTextFile(false);
            } else if (MIMETypeRegistry::isSupportedJavaScriptMIMEType(mimeType)) {
                resourceType = ResourceType::Script;
                entryForFrontend->setIsTextFile(true);
            } else if (MIMETypeRegistry::isSupportedNonImageMIMEType(mimeType)) {
                resourceType = ResourceType::Document;
                entryForFrontend->setIsTextFile(true);
            } else {
                resourceType = ResourceType::Other;
                entryForFrontend->setIsTextFile(DOMImplementation::isXMLMIMEType(mimeType) || DOMImplementation::isTextMIMEType(mimeType));
            }

            entryForFrontend->setMimeType(mimeType);
            entryForFrontend->setResourceType(resourceType);
        }

        m_entries->addItem(entryForFrontend);
    }
    readDirectoryEntries();
    return true;
}

class MetadataRequest : public RefCounted<MetadataRequest> {
    WTF_MAKE_NONCOPYABLE(MetadataRequest);
public:
    static PassRefPtr<MetadataRequest> create(PassRefPtr<RequestMetadataCallback> requestCallback, const String& url)
    {
        return adoptRef(new MetadataRequest(requestCallback, url));
    }

    virtual ~MetadataRequest()
    {
        reportResult(FileError::ABORT_ERR);
    }

    void start(ExecutionContext*);

private:
    bool didHitError(FileError* error)
    {
        reportResult(error->code());
        return true;
    }

    bool didGetEntry(Entry*);
    bool didGetMetadata(Metadata*);

    void reportResult(FileError::ErrorCode errorCode, PassRefPtr<TypeBuilder::FileSystem::Metadata> metadata = 0)
    {
        m_requestCallback->sendSuccess(static_cast<int>(errorCode), metadata);
    }

    MetadataRequest(PassRefPtr<RequestMetadataCallback> requestCallback, const String& url)
        : m_requestCallback(requestCallback)
        , m_url(ParsedURLString, url) { }

    RefPtr<RequestMetadataCallback> m_requestCallback;
    KURL m_url;
    bool m_isDirectory;
};

void MetadataRequest::start(ExecutionContext* executionContext)
{
    ASSERT(executionContext);

    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &MetadataRequest::didHitError);
    RefPtr<EntryCallback> successCallback = CallbackDispatcherFactory<EntryCallback>::create(this, &MetadataRequest::didGetEntry);
    OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = ResolveURICallbacks::create(successCallback, errorCallback, executionContext);
    LocalFileSystem::from(executionContext)->resolveURL(executionContext, m_url, fileSystemCallbacks.release());
}

bool MetadataRequest::didGetEntry(Entry* entry)
{
    if (!entry->filesystem()->executionContext()) {
        reportResult(FileError::ABORT_ERR);
        return true;
    }

    RefPtr<MetadataCallback> successCallback = CallbackDispatcherFactory<MetadataCallback>::create(this, &MetadataRequest::didGetMetadata);
    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &MetadataRequest::didHitError);
    entry->getMetadata(successCallback, errorCallback);
    m_isDirectory = entry->isDirectory();
    return true;
}

bool MetadataRequest::didGetMetadata(Metadata* metadata)
{
    using TypeBuilder::FileSystem::Metadata;
    RefPtr<Metadata> result = Metadata::create()
        .setModificationTime(metadata->modificationTime())
        .setSize(metadata->size());
    reportResult(static_cast<FileError::ErrorCode>(0), result);
    return true;
}

class FileContentRequest : public EventListener {
    WTF_MAKE_NONCOPYABLE(FileContentRequest);
public:
    static PassRefPtr<FileContentRequest> create(PassRefPtr<RequestFileContentCallback> requestCallback, const String& url, bool readAsText, long long start, long long end, const String& charset)
    {
        return adoptRef(new FileContentRequest(requestCallback, url, readAsText, start, end, charset));
    }

    virtual ~FileContentRequest()
    {
        reportResult(FileError::ABORT_ERR);
    }

    void start(ExecutionContext*);

    virtual bool operator==(const EventListener& other) OVERRIDE
    {
        return this == &other;
    }

    virtual void handleEvent(ExecutionContext*, Event* event) OVERRIDE
    {
        if (event->type() == EventTypeNames::load)
            didRead();
        else if (event->type() == EventTypeNames::error)
            didHitError(m_reader->error().get());
    }

private:
    bool didHitError(FileError* error)
    {
        reportResult(error->code());
        return true;
    }

    bool didGetEntry(Entry*);
    bool didGetFile(File*);
    void didRead();

    void reportResult(FileError::ErrorCode errorCode, const String* result = 0, const String* charset = 0)
    {
        m_requestCallback->sendSuccess(static_cast<int>(errorCode), result, charset);
    }

    FileContentRequest(PassRefPtr<RequestFileContentCallback> requestCallback, const String& url, bool readAsText, long long start, long long end, const String& charset)
        : EventListener(EventListener::CPPEventListenerType)
        , m_requestCallback(requestCallback)
        , m_url(ParsedURLString, url)
        , m_readAsText(readAsText)
        , m_start(start)
        , m_end(end)
        , m_charset(charset) { }

    RefPtr<RequestFileContentCallback> m_requestCallback;
    KURL m_url;
    bool m_readAsText;
    int m_start;
    long long m_end;
    String m_mimeType;
    String m_charset;

    RefPtr<FileReader> m_reader;
};

void FileContentRequest::start(ExecutionContext* executionContext)
{
    ASSERT(executionContext);

    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &FileContentRequest::didHitError);
    RefPtr<EntryCallback> successCallback = CallbackDispatcherFactory<EntryCallback>::create(this, &FileContentRequest::didGetEntry);

    OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = ResolveURICallbacks::create(successCallback, errorCallback, executionContext);
    LocalFileSystem::from(executionContext)->resolveURL(executionContext, m_url, fileSystemCallbacks.release());
}

bool FileContentRequest::didGetEntry(Entry* entry)
{
    if (entry->isDirectory()) {
        reportResult(FileError::TYPE_MISMATCH_ERR);
        return true;
    }

    if (!entry->filesystem()->executionContext()) {
        reportResult(FileError::ABORT_ERR);
        return true;
    }

    RefPtr<FileCallback> successCallback = CallbackDispatcherFactory<FileCallback>::create(this, &FileContentRequest::didGetFile);
    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &FileContentRequest::didHitError);
    static_cast<FileEntry*>(entry)->file(successCallback, errorCallback);

    m_reader = FileReader::create(entry->filesystem()->executionContext());
    m_mimeType = MIMETypeRegistry::getMIMETypeForPath(entry->name());

    return true;
}

bool FileContentRequest::didGetFile(File* file)
{
    RefPtr<Blob> blob = file->slice(m_start, m_end);
    m_reader->setOnload(this);
    m_reader->setOnerror(this);

    m_reader->readAsArrayBuffer(blob.get(), IGNORE_EXCEPTION);
    return true;
}

void FileContentRequest::didRead()
{
    RefPtr<ArrayBuffer> buffer = m_reader->arrayBufferResult();

    if (!m_readAsText) {
        String result = base64Encode(static_cast<char*>(buffer->data()), buffer->byteLength());
        reportResult(static_cast<FileError::ErrorCode>(0), &result, 0);
        return;
    }

    RefPtr<TextResourceDecoder> decoder = TextResourceDecoder::create(m_mimeType, m_charset, true);
    String result = decoder->decode(static_cast<char*>(buffer->data()), buffer->byteLength());
    result.append(decoder->flush());
    m_charset = decoder->encoding().name();
    reportResult(static_cast<FileError::ErrorCode>(0), &result, &m_charset);
}

class DeleteEntryRequest : public VoidCallback {
public:
    static PassRefPtr<DeleteEntryRequest> create(PassRefPtr<DeleteEntryCallback> requestCallback, const KURL& url)
    {
        return adoptRef(new DeleteEntryRequest(requestCallback, url));
    }

    virtual ~DeleteEntryRequest()
    {
        reportResult(FileError::ABORT_ERR);
    }

    virtual bool handleEvent() OVERRIDE
    {
        return didDeleteEntry();
    }

    void start(ExecutionContext*);

private:
    bool didHitError(FileError* error)
    {
        reportResult(error->code());
        return true;
    }

    bool didGetEntry(Entry*);
    bool didDeleteEntry();

    void reportResult(FileError::ErrorCode errorCode)
    {
        m_requestCallback->sendSuccess(static_cast<int>(errorCode));
    }

    DeleteEntryRequest(PassRefPtr<DeleteEntryCallback> requestCallback, const KURL& url)
        : m_requestCallback(requestCallback)
        , m_url(url) { }

    RefPtr<DeleteEntryCallback> m_requestCallback;
    KURL m_url;
};

void DeleteEntryRequest::start(ExecutionContext* executionContext)
{
    ASSERT(executionContext);

    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &DeleteEntryRequest::didHitError);

    FileSystemType type;
    String path;
    if (!DOMFileSystemBase::crackFileSystemURL(m_url, type, path)) {
        errorCallback->handleEvent(FileError::create(FileError::SYNTAX_ERR).get());
        return;
    }

    if (path == "/") {
        OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = VoidCallbacks::create(this, errorCallback, 0);
        LocalFileSystem::from(executionContext)->deleteFileSystem(executionContext, type, fileSystemCallbacks.release());
    } else {
        RefPtr<EntryCallback> successCallback = CallbackDispatcherFactory<EntryCallback>::create(this, &DeleteEntryRequest::didGetEntry);
        OwnPtr<AsyncFileSystemCallbacks> fileSystemCallbacks = ResolveURICallbacks::create(successCallback, errorCallback, executionContext);
        LocalFileSystem::from(executionContext)->resolveURL(executionContext, m_url, fileSystemCallbacks.release());
    }
}

bool DeleteEntryRequest::didGetEntry(Entry* entry)
{
    RefPtr<ErrorCallback> errorCallback = CallbackDispatcherFactory<ErrorCallback>::create(this, &DeleteEntryRequest::didHitError);
    if (entry->isDirectory()) {
        DirectoryEntry* directoryEntry = static_cast<DirectoryEntry*>(entry);
        directoryEntry->removeRecursively(this, errorCallback);
    } else
        entry->remove(this, errorCallback);
    return true;
}

bool DeleteEntryRequest::didDeleteEntry()
{
    reportResult(static_cast<FileError::ErrorCode>(0));
    return true;
}

} // anonymous namespace

// static
PassOwnPtr<InspectorFileSystemAgent> InspectorFileSystemAgent::create(InstrumentingAgents* instrumentingAgents, InspectorPageAgent* pageAgent, InspectorCompositeState* state)
{
    return adoptPtr(new InspectorFileSystemAgent(instrumentingAgents, pageAgent, state));
}

InspectorFileSystemAgent::~InspectorFileSystemAgent()
{
}

void InspectorFileSystemAgent::enable(ErrorString*)
{
    if (m_enabled)
        return;
    m_enabled = true;
    m_state->setBoolean(FileSystemAgentState::fileSystemAgentEnabled, m_enabled);
}

void InspectorFileSystemAgent::disable(ErrorString*)
{
    if (!m_enabled)
        return;
    m_enabled = false;
    m_state->setBoolean(FileSystemAgentState::fileSystemAgentEnabled, m_enabled);
}

void InspectorFileSystemAgent::requestFileSystemRoot(ErrorString* error, const String& origin, const String& type, PassRefPtr<RequestFileSystemRootCallback> requestCallback)
{
    if (!assertEnabled(error))
        return;

    ExecutionContext* executionContext = assertExecutionContextForOrigin(error, SecurityOrigin::createFromString(origin).get());
    if (!executionContext)
        return;

    FileSystemRootRequest::create(requestCallback, type)->start(executionContext);
}

void InspectorFileSystemAgent::requestDirectoryContent(ErrorString* error, const String& url, PassRefPtr<RequestDirectoryContentCallback> requestCallback)
{
    if (!assertEnabled(error))
        return;

    ExecutionContext* executionContext = assertExecutionContextForOrigin(error, SecurityOrigin::createFromString(url).get());
    if (!executionContext)
        return;

    DirectoryContentRequest::create(requestCallback, url)->start(executionContext);
}

void InspectorFileSystemAgent::requestMetadata(ErrorString* error, const String& url, PassRefPtr<RequestMetadataCallback> requestCallback)
{
    if (!assertEnabled(error))
        return;

    ExecutionContext* executionContext = assertExecutionContextForOrigin(error, SecurityOrigin::createFromString(url).get());
    if (!executionContext)
        return;

    MetadataRequest::create(requestCallback, url)->start(executionContext);
}

void InspectorFileSystemAgent::requestFileContent(ErrorString* error, const String& url, bool readAsText, const int* start, const int* end, const String* charset, PassRefPtr<RequestFileContentCallback> requestCallback)
{
    if (!assertEnabled(error))
        return;

    ExecutionContext* executionContext = assertExecutionContextForOrigin(error, SecurityOrigin::createFromString(url).get());
    if (!executionContext)
        return;

    long long startPosition = start ? *start : 0;
    long long endPosition = end ? *end : std::numeric_limits<long long>::max();
    FileContentRequest::create(requestCallback, url, readAsText, startPosition, endPosition, charset ? *charset : "")->start(executionContext);
}

void InspectorFileSystemAgent::deleteEntry(ErrorString* error, const String& urlString, PassRefPtr<DeleteEntryCallback> requestCallback)
{
    if (!assertEnabled(error))
        return;

    KURL url(ParsedURLString, urlString);

    ExecutionContext* executionContext = assertExecutionContextForOrigin(error, SecurityOrigin::create(url).get());
    if (!executionContext)
        return;

    DeleteEntryRequest::create(requestCallback, url)->start(executionContext);
}

void InspectorFileSystemAgent::clearFrontend()
{
    m_enabled = false;
    m_state->setBoolean(FileSystemAgentState::fileSystemAgentEnabled, m_enabled);
}

void InspectorFileSystemAgent::restore()
{
    m_enabled = m_state->getBoolean(FileSystemAgentState::fileSystemAgentEnabled);
}

InspectorFileSystemAgent::InspectorFileSystemAgent(InstrumentingAgents* instrumentingAgents, InspectorPageAgent* pageAgent, InspectorCompositeState* state)
    : InspectorBaseAgent<InspectorFileSystemAgent>("FileSystem", instrumentingAgents, state)
    , m_pageAgent(pageAgent)
    , m_enabled(false)
{
    ASSERT(instrumentingAgents);
    ASSERT(state);
    ASSERT(m_pageAgent);
}

bool InspectorFileSystemAgent::assertEnabled(ErrorString* error)
{
    if (!m_enabled) {
        *error = "FileSystem agent is not enabled.";
        return false;
    }
    return true;
}

ExecutionContext* InspectorFileSystemAgent::assertExecutionContextForOrigin(ErrorString* error, SecurityOrigin* origin)
{
    for (Frame* frame = m_pageAgent->mainFrame(); frame; frame = frame->tree().traverseNext()) {
        if (frame->document() && frame->document()->securityOrigin()->isSameSchemeHostPort(origin))
            return frame->document();
    }

    *error = "No frame is available for the request";
    return 0;
}

} // namespace WebCore