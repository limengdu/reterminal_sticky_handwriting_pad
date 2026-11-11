#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(a,e,...) do {if (!(a)) return (e);} while(0)
#define ESP_RETURN_ON_ERROR(a,...) do {int mock_err=(a); if(mock_err!=ESP_OK) return mock_err;} while(0)
