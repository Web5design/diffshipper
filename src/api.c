#include <curl/curl.h>
#include <curl/easy.h>

#include "api.h"
#include "options.h"

int api_init() {
    return curl_global_init(CURL_GLOBAL_ALL);
}


int api_create_room() {
    
    return 0;
}
