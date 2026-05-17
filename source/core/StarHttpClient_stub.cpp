#include "StarHttpClient.hpp"
#include "StarWorkerPool.hpp"

namespace Star {

WorkerPool& HttpClient::workerPool() {
  static WorkerPool pool("HttpClient", 1);
  return pool;
}

HttpClient::HttpClient() = default;

HttpClient::~HttpClient() = default;

WorkerPoolPromise<HttpResponse> HttpClient::requestAsync(HttpRequest const& request) {
  return workerPool().addProducer<HttpResponse>([request]() {
    HttpResponse response;
    // STUB: HTTP client backend is intentionally disabled for Nintendo 3DS phase1.
    response.error = String::joinWith("", "HTTP service is unavailable on this platform placeholder path (", request.method, " ", request.url, ")");
    return response;
  });
}

WorkerPoolPromise<HttpResponse> HttpClient::getAsync(String const& url, StringMap<String> const& headers) {
  return requestAsync(HttpRequest{"GET", url, headers, {}});
}

WorkerPoolPromise<HttpResponse> HttpClient::postAsync(String const& url, String const& body, StringMap<String> const& headers) {
  return requestAsync(HttpRequest{"POST", url, headers, body});
}

WorkerPoolPromise<HttpResponse> HttpClient::putAsync(String const& url, String const& body, StringMap<String> const& headers) {
  return requestAsync(HttpRequest{"PUT", url, headers, body});
}

WorkerPoolPromise<HttpResponse> HttpClient::deleteAsync(String const& url, StringMap<String> const& headers) {
  return requestAsync(HttpRequest{"DELETE", url, headers, {}});
}

WorkerPoolPromise<HttpResponse> HttpClient::patchAsync(String const& url, String const& body, StringMap<String> const& headers) {
  return requestAsync(HttpRequest{"PATCH", url, headers, body});
}

}
