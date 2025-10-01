#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>

// --- USER CONFIGURATION: EDIT THESE THREE LINES ---
// 1. Set the full URL to your Open WebUI chat completions API endpoint.
#define WEBUI_URL "http://your-server-ip:8080/api/v1/chat/completions"

// 2. Set the exact model name as it appears in Open WebUI.
#define WEBUI_MODEL "gpt-oss:latest"

// 3. Set your secret API key, prefixed with "Bearer ".
#define WEBUI_API_KEY "Bearer your_secret_api_key_here"
// ----------------------------------------------------


// The System Prompt defines the AI's core instructions and persona.
#define SYSTEM_PROMPT "You are a methodical Linux agent. Your goal is to accomplish the user's request by creating and executing a logical sequence of actions. Your response MUST be a single, raw JSON object for the next action.\n\n**Your Constitution:**\n1.  **Think Step-by-Step:** Break down complex goals into a series of simple, single-purpose steps. Do not chain unrelated commands.\n2.  **Proceed Logically:** After creating a resource (like a file), the next logical step is to USE it (e.g., compile it). After installing a tool, the next logical step is to USE that tool.\n3.  **Use Privileges Correctly:** Always prepend 'sudo' to any command that requires root privileges (like 'apt', 'dpkg', 'systemctl').\n4.  **Stay Focused:** Always refer back to the user's original goal to guide your next step.\n\n**Actions:**\n1. `execute_command`: {\"action\": \"execute_command\", \"parameters\": {\"command\": \"<shell_command>\"}}\n2. `write_file`: {\"action\": \"write_file\", \"parameters\": {\"filename\": \"<file_name>\", \"content\": \"<file_content>\"}}\n3. `ask_user`: {\"action\": \"ask_user\", \"parameters\": {\"question\": \"<your question>\"}}\n4. `answer_user`: {\"action\": \"answer_user\", \"parameters\": {\"answer\": \"<final report>\"}}\n\nGenerate the JSON for the single next action: "

// The User Prompt Template combines the user's goal with the context from the last action.
#define USER_PROMPT_TEMPLATE "**USER'S GOAL:** \"%s\"\n\n**LAST STEP'S OUTPUT:**\n---\n%s\n---\n\nBased on your constitution, the user's goal, and the last step's output, determine the single next action. Only use 'answer_user' when the goal is 100%% complete."


typedef struct { char *action, *command, *filename, *content, *answer, *question; } AIAction;
struct MemoryStruct { char *memory; size_t size; };

static char* normalize_command(const char *command) {
    if (!command) return NULL;

    size_t len = strlen(command);
    char *compressed = malloc(len + 1);
    if (!compressed) return NULL;

    size_t write_idx = 0;
    int seen_space = 1;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)command[i];
        if (isspace(c)) {
            if (!seen_space) {
                compressed[write_idx++] = ' ';
                seen_space = 1;
            }
        } else {
            compressed[write_idx++] = c;
            seen_space = 0;
        }
    }
    if (write_idx > 0 && compressed[write_idx - 1] == ' ') write_idx--;
    compressed[write_idx] = '\0';

    char *normalized = malloc(write_idx + 1);
    if (!normalized) {
        free(compressed);
        return NULL;
    }

    size_t norm_idx = 0;
    for (size_t i = 0; i < write_idx; ) {
        if (i + 7 <= write_idx && strncmp(&compressed[i], "apt-get", 7) == 0) {
            memcpy(&normalized[norm_idx], "apt", 3);
            norm_idx += 3;
            i += 7;
        } else {
            normalized[norm_idx++] = compressed[i++];
        }
    }
    normalized[norm_idx] = '\0';

    free(compressed);
    return normalized;
}

static char* build_write_context(const char *filename, const char *content) {
    size_t content_len = content ? strlen(content) : 0;
    const size_t preview_cap = 80;
    size_t preview_len = content_len < preview_cap ? content_len : preview_cap;
    char *preview = malloc(preview_len + 1);
    if (!preview) return NULL;

    for (size_t i = 0; i < preview_len; ++i) {
        char c = content[i];
        if (c == '\n' || c == '\r' || c == '\t') preview[i] = ' ';
        else preview[i] = c;
    }
    preview[preview_len] = '\0';

    char *context_message = NULL;
    if (asprintf(&context_message, "Wrote %zu bytes to %s. Preview: \"%s%s\"", content_len, filename, preview, content_len > preview_len ? "..." : "") == -1) {
        context_message = NULL;
    }

    free(preview);
    return context_message ? context_message : strdup("File written successfully.");
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) { printf("Error: not enough memory\n"); return 0; }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

char* execute_and_capture(const char* command) {
    FILE *fp;
    char buffer[1024];
    char *output = malloc(1);
    if (!output) {
        fprintf(stderr, "Error: unable to allocate memory for command output.\n");
        return strdup("Command output unavailable: memory allocation failed.");
    }
    output[0] = '\0';
    size_t output_size = 1;
    char full_command[2048];
    snprintf(full_command, sizeof(full_command), "%s 2>&1", command);
    fp = popen(full_command, "r");
    if (fp == NULL) { fprintf(stderr, "Failed to run command\n"); return output; }
    printf("--- Output ---\n");
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        printf("%s", buffer);
        size_t buffer_len = strlen(buffer);
        char *ptr = realloc(output, output_size + buffer_len);
        if(!ptr) { fprintf(stderr, "Failed to realloc memory\n"); break; }
        output = ptr;
        strcat(output, buffer);
        output_size += buffer_len;
    }
    printf("--------------\n");
    pclose(fp);
    return output;
}

void parse_webui_action(const char *json_string, AIAction *ai_action) {
    memset(ai_action, 0, sizeof(AIAction));
    struct json_object *parsed_json = json_tokener_parse(json_string);
    if (!parsed_json) {
        fprintf(stderr, "Error: Failed to parse JSON response.\n");
        return;
    }

    struct json_object *choices_array, *first_choice, *message_obj, *content_obj;
    if (!json_object_object_get_ex(parsed_json, "choices", &choices_array)) {
        fprintf(stderr, "Error: 'choices' key not found in response.\n");
        json_object_put(parsed_json);
        return;
    }

    if (json_object_get_type(choices_array) != json_type_array || json_object_array_length(choices_array) == 0) {
        fprintf(stderr, "Error: 'choices' is not a valid array or is empty.\n");
        json_object_put(parsed_json);
        return;
    }

    first_choice = json_object_array_get_idx(choices_array, 0);
    if (!json_object_object_get_ex(first_choice, "message", &message_obj)) {
        fprintf(stderr, "Error: 'message' key not found in first choice.\n");
        json_object_put(parsed_json);
        return;
    }
    
    if (!json_object_object_get_ex(message_obj, "content", &content_obj)) {
        fprintf(stderr, "Error: 'content' key not found in message.\n");
        json_object_put(parsed_json);
        return;
    }

    const char *response_str = json_object_get_string(content_obj);
    
    struct json_object *action_json = json_tokener_parse(response_str);
    if (!action_json) { 
        fprintf(stderr, "Error: Failed to parse the action JSON from the 'content' field.\nContent was: %s\n", response_str);
        json_object_put(parsed_json); 
        return;
    }

    struct json_object *type_obj, *params_obj, *temp_obj;
    if (json_object_object_get_ex(action_json, "action", &type_obj)) {
        ai_action->action = strdup(json_object_get_string(type_obj));
    }
    if (ai_action->action && json_object_object_get_ex(action_json, "parameters", &params_obj)) {
        if (strcmp(ai_action->action, "execute_command") == 0) {
            if (json_object_object_get_ex(params_obj, "command", &temp_obj))
                ai_action->command = strdup(json_object_get_string(temp_obj));
        } else if (strcmp(ai_action->action, "write_file") == 0) {
            if (json_object_object_get_ex(params_obj, "filename", &temp_obj))
                ai_action->filename = strdup(json_object_get_string(temp_obj));
            if (json_object_object_get_ex(params_obj, "content", &temp_obj))
                ai_action->content = strdup(json_object_get_string(temp_obj));
        } else if (strcmp(ai_action->action, "answer_user") == 0) {
            if (json_object_object_get_ex(params_obj, "answer", &temp_obj))
                ai_action->answer = strdup(json_object_get_string(temp_obj));
        } else if (strcmp(ai_action->action, "ask_user") == 0) {
            if (json_object_object_get_ex(params_obj, "question", &temp_obj))
                ai_action->question = strdup(json_object_get_string(temp_obj));
        }
    }
    json_object_put(action_json);
    json_object_put(parsed_json);
}

void get_ai_action(const char* user_prompt, AIAction *ai_action) {
    memset(ai_action, 0, sizeof(*ai_action));
    CURL *curl;
    struct MemoryStruct chunk = { .memory = malloc(1), .size = 0 };
    if (!chunk.memory) {
        fprintf(stderr, "Error: unable to allocate memory for response buffer.\n");
        return;
    }
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (curl) {
        json_object *jobj = json_object_new_object();
        json_object_object_add(jobj, "model", json_object_new_string(WEBUI_MODEL));

        json_object *messages_array = json_object_new_array();

        json_object *system_message = json_object_new_object();
        json_object_object_add(system_message, "role", json_object_new_string("system"));
        json_object_object_add(system_message, "content", json_object_new_string(SYSTEM_PROMPT));
        json_object_array_add(messages_array, system_message);

        json_object *user_message = json_object_new_object();
        json_object_object_add(user_message, "role", json_object_new_string("user"));
        json_object_object_add(user_message, "content", json_object_new_string(user_prompt));
        json_object_array_add(messages_array, user_message);

        json_object_object_add(jobj, "messages", messages_array);
        json_object_object_add(jobj, "stream", json_object_new_boolean(0));

        const char *json_payload = json_object_to_json_string(jobj);
        
        char auth_header[4096];
        snprintf(auth_header, sizeof(auth_header), "Authorization: %s", WEBUI_API_KEY);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth_header);
        
        curl_easy_setopt(curl, CURLOPT_URL, WEBUI_URL);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        
        printf("🧠 Thinking... "); fflush(stdout);
        CURLcode res = curl_easy_perform(curl);
        printf("Done.\n");

        if (res == CURLE_OK) {
            parse_webui_action(chunk.memory, ai_action);
        } else {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        json_object_put(jobj);
    }
    curl_global_cleanup();
    free(chunk.memory);
}

void free_ai_action(AIAction *action) {
    if (action->action) free(action->action);
    if (action->command) free(action->command);
    if (action->filename) free(action->filename);
    if (action->content) free(action->content);
    if (action->answer) free(action->answer);
    if (action->question) free(action->question);
}

int main() {
    char input[4096];
    printf("Welcome to Smart Agent Terminal ✨\n");
    printf("State your goal starting with '\"' (e.g., \"is a webserver running?\").\n");
    printf("--------------------------------------------------------------------------------\n");

    while (1) {
        printf(">> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) continue;
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) break;

        if (input[0] == '"') {
            char *goal = strdup(input + 1);
            char *last_char = strrchr(goal, '"');
            if (last_char) *last_char = '\0';

            char *context = strdup("No commands executed yet. This is the first step.");
            
            const int history_size = 3;
            char *command_history[history_size];
            for(int i=0; i<history_size; i++) command_history[i] = NULL;
            char *last_written_filename = NULL;
            char *last_written_content = NULL;
            int command_loop_warning_count = 0;
            int write_loop_warning_count = 0;
            const int max_loop_warnings = 2;

            for (int loop_count = 0; loop_count < 15; loop_count++) { // Safety break
                char full_prompt[16384];
                snprintf(full_prompt, sizeof(full_prompt), USER_PROMPT_TEMPLATE, goal, context);
                
                AIAction action;
                get_ai_action(full_prompt, &action);

                free(context);
                context = NULL;

                if (!action.action) {
                    printf("🤖 AI failed to provide a valid action. Aborting.\n");
                    break;
                }

                if (strcmp(action.action, "execute_command") == 0 && action.command) {
                    int is_stuck = 0;
                    char *normalized_command = normalize_command(action.command);
                    for(int i=0; i<history_size; i++) {
                        if(command_history[i] && normalized_command && strcmp(command_history[i], normalized_command) == 0) {
                            is_stuck = 1;
                            break;
                        }
                    }
                    if(is_stuck) {
                        command_loop_warning_count++;
                        printf("🤖 AI attempted to repeat the command: %s\n", action.command);
                        if (command_loop_warning_count >= max_loop_warnings) {
                            printf("🤖 Repeated command limit reached. Aborting automatic execution.\n");
                            if (context) free(context);
                            context = strdup("Automatic execution stopped: the same command was requested multiple times without progress.");
                            free(normalized_command);
                            free_ai_action(&action);
                            break;
                        }

                        if (context) free(context);
                        if (normalized_command) {
                            if (asprintf(&context, "Command repetition detected for: %s. Choose a different action or provide the final answer.", normalized_command) == -1) {
                                context = strdup("Command repetition detected. Choose a different action or provide the final answer.");
                            }
                        } else {
                            context = strdup("Command repetition detected. Choose a different action or provide the final answer.");
                        }
                        free(normalized_command);
                        free_ai_action(&action);
                        continue;
                    }
                    if(command_history[history_size-1]) free(command_history[history_size-1]);
                    for(int i=history_size-1; i > 0; i--) command_history[i] = command_history[i-1];
                    command_history[0] = normalized_command ? normalized_command : strdup(action.command);
                    printf("🤖 Step %d: Running command: \033[1;33m%s\033[0m\n", loop_count + 1, action.command);
                    context = execute_and_capture(action.command);

                } else if (strcmp(action.action, "write_file") == 0 && action.filename && action.content) {
                    printf("🤖 Step %d: Writing to file: \033[1;33m%s\033[0m\n", loop_count + 1, action.filename);
                    if (last_written_filename && last_written_content &&
                        strcmp(last_written_filename, action.filename) == 0 &&
                        strcmp(last_written_content, action.content) == 0) {
                        write_loop_warning_count++;
                        printf("🤖 Detected repeated write to %s with identical content.\n", action.filename);
                        if (write_loop_warning_count >= max_loop_warnings) {
                            printf("🤖 Repeated file write limit reached. Aborting automatic execution.\n");
                            if (context) free(context);
                            context = strdup("Automatic execution stopped: repeated identical file writes detected multiple times.");
                            free_ai_action(&action);
                            break;
                        }

                        if (context) free(context);
                        if (asprintf(&context, "Repeated identical write detected for %s. Choose a different action or finalize the task.", action.filename) == -1) {
                            context = strdup("Repeated identical file write detected. Choose a different action or finalize the task.");
                        }
                        free_ai_action(&action);
                        continue;
                    }

                    FILE *fp = fopen(action.filename, "w");
                    if (fp) {
                        fprintf(fp, "%s", action.content);
                        fclose(fp);
                        if (last_written_filename) free(last_written_filename);
                        if (last_written_content) free(last_written_content);
                        last_written_filename = strdup(action.filename);
                        last_written_content = strdup(action.content);
                        context = build_write_context(action.filename, action.content);
                        if (!context) context = strdup("File written successfully.");
                        printf("✅ %s\n", context);
                    } else {
                        context = strdup("Error opening file for writing.");
                        fprintf(stderr, "Error: %s\n", context);
                    }
                } else if (strcmp(action.action, "ask_user") == 0 && action.question) {
                    printf("🤖 \033[1;34mAI Question: %s\033[0m\n", action.question);
                    printf("Your reply: ");
                    char reply[1024];
                    if(fgets(reply, sizeof(reply), stdin) != NULL) {
                        reply[strcspn(reply, "\n")] = 0;
                        context = strdup(reply);
                    } else {
                        context = strdup("User provided no reply.");
                    }
                }
                else if (strcmp(action.action, "answer_user") == 0 && action.answer) {
                    printf("✅ \033[1;32mAI Final Answer: %s\033[0m\n", action.answer);
                    free_ai_action(&action);
                    break; 
                } else {
                    printf("🤖 AI returned an invalid or empty action. Aborting.\n");
                    free_ai_action(&action);
                    break;
                }
                
                free_ai_action(&action);
            }

            if (context) free(context);
            free(goal);
            for(int i=0; i<history_size; i++) if(command_history[i]) free(command_history[i]);
            if (last_written_filename) free(last_written_filename);
            if (last_written_content) free(last_written_content);
        } else {
            int return_code = system(input);
            if (return_code != 0) {
                fprintf(stderr, "Command finished with a non-zero exit code: %d\n", return_code);
            }
        }
    }
    printf("\nExiting Smart Agent. Goodbye!\n");
    return 0;
}
