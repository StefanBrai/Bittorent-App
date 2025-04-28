#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 50
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_PEERS 100
#define SEGMENT_REQUEST_BATCH 10

// Definirea etichetelor de mesaje
#define MSG_INIT 1
#define MSG_UPLOAD 2
#define MSG_ACK 3
#define MSG_LIST_PEERS 4
#define MSG_PEER_LIST 5
#define MSG_DOWNLOAD_REQUEST 6
#define MSG_DOWNLOAD_RESPONSE 7
#define MSG_FINISH_DOWNLOAD 8
#define MSG_FINALIZE_ALL 9
#define MSG_END_UPLOAD 10
#define MSG_START_DOWNLOAD 11
#define MSG_RECEIVED_SEGMENT 12

// Structura detaliilor despre fisierele trackerului
typedef struct
{
    char filename[MAX_FILENAME];
    int total_segments;
    char segment_hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int clients_with_file[MAX_PEERS]; // Lista clientilor care detin fisierul
} TrackerFile;

// Structura informatiilor despre un fisier
typedef struct
{
    char filename[MAX_FILENAME];
    int total_segments;
    char segments[MAX_CHUNKS][HASH_SIZE + 1];
} FileDetails;

// Structura informatiilor pe care le are un peer
typedef struct
{
    FileDetails owned_files[MAX_FILES];
    int owned_file_count;
    char requested_files[MAX_FILES][MAX_FILENAME];
    int requested_file_count;
} PeerInfo;

// Structura argumentelor pentru firele de upload și download
typedef struct
{
    int rank;
    PeerInfo *peer_info;
    pthread_mutex_t *peer_info_mutex;
} ThreadArgs;

// Structura informatiilor despre descărcare
typedef struct
{
    char filename[MAX_FILENAME];
    int total_segments;
    int segments_downloaded;
    int segments_total;
    int peers[MAX_PEERS];
    int peer_count;
    char filename_hashes[MAX_CHUNKS][HASH_SIZE + 1];
} DownloadInfo;

// Variabile globale
TrackerFile tracker_files[MAX_FILES];
int tracker_file_count = 0;

PeerInfo global_peer_info;

// **Log file global**
FILE *log_file = NULL;

// Functia trackerului
void tracker(int numtasks, int rank)
{
    tracker_file_count = 0;
    memset(tracker_files, 0, sizeof(tracker_files));
    MPI_Status status;

    int clients_finalized = 0; // numar de clienti care au trimis FINALIZE_ALL
    int expected_inits = numtasks - 1;
    int received_inits = 0;
    int pending_segments[MAX_PEERS] = {0}; // Segmente asteptate de la fiecare peer

    // ---------------- Faza 1: Initializarea fiecarui peer ---------------
    while (received_inits < expected_inits)
    {
        // Asteptam un mesaj
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int message_size;
        MPI_Get_count(&status, MPI_CHAR, &message_size);

        char *message = (char *)malloc(message_size + 1);
        MPI_Recv(message, message_size, MPI_CHAR,
                 status.MPI_SOURCE, status.MPI_TAG,
                 MPI_COMM_WORLD, &status);
        message[message_size] = '\0';

        int sender_rank = status.MPI_SOURCE;
        fprintf(stdout, "Tracker: Received message of size %d from rank %d with tag %d\n",
                message_size, sender_rank, status.MPI_TAG);
        fflush(stdout);

        if (status.MPI_TAG == MSG_INIT)
        {
            // Gestionarea mesajului INIT

            char *ptr = strtok(message, " "); // "INIT"
            ptr = strtok(NULL, " ");
            int num_files = atoi(ptr);
            int total_segments = 0;

            for (int i = 0; i < num_files; i++)
            {
                // Procesam fiecare fisier
                ptr = strtok(NULL, " ");
                char filename[MAX_FILENAME];
                strcpy(filename, ptr);

                ptr = strtok(NULL, " ");
                int seg_count = atoi(ptr);

                total_segments += seg_count;

                int file_index = -1;
                for (int j = 0; j < tracker_file_count; j++)
                {
                    if (strcmp(tracker_files[j].filename, filename) == 0)
                    {
                        file_index = j;
                        break;
                    }
                }

                if (file_index == -1)
                {
                    file_index = tracker_file_count++;
                    strcpy(tracker_files[file_index].filename, filename);
                    tracker_files[file_index].total_segments = 0;
                    memset(tracker_files[file_index].clients_with_file, 0,
                           sizeof(tracker_files[file_index].clients_with_file));
                }

                tracker_files[file_index].clients_with_file[sender_rank] = 1;

                if (seg_count > tracker_files[file_index].total_segments)
                {
                    tracker_files[file_index].total_segments = seg_count;
                }
            }
            if (total_segments == 0)
            {
                received_inits += 1;
            }
            pending_segments[sender_rank] = total_segments;
        }
        else if (status.MPI_TAG == MSG_UPLOAD)
        {
            // Gestionarea mesajului UPLOAD
            char filename[MAX_FILENAME];
            char hash_value[HASH_SIZE + 1];
            int segment_index;

            sscanf(message, "UPLOAD %s %d %s", filename, &segment_index, hash_value);

            int file_index = -1;
            for (int i = 0; i < tracker_file_count; i++)
            {
                if (strcmp(tracker_files[i].filename, filename) == 0)
                {
                    file_index = i;
                    break;
                }
            }

            if (file_index != -1 && segment_index < tracker_files[file_index].total_segments)
            {
                strcpy(tracker_files[file_index].segment_hashes[segment_index], hash_value);
                fprintf(log_file, "Tracker: Stored hash for file %s, segment %d, hash %s.\n",
                        filename, segment_index, hash_value);
                fflush(log_file);
            }

            pending_segments[sender_rank]--;

            if (pending_segments[sender_rank] == 0)
            {

                received_inits += 1;
            }
        }
    }

    for (int p = 1; p < numtasks; p++)
    {
        MPI_Send("ACK", 4, MPI_CHAR, p, MSG_ACK, MPI_COMM_WORLD);
        fprintf(log_file, "Tracker: Sent ACK to Peer %d for INIT.\n", p);
        fflush(log_file);
    }

    // ---------------- Faza 2: Asistarea fiecarui peer cu informatii despre fisiere si cu mesaj de finalizare ---------------

    while (clients_finalized < numtasks - 1)
    {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int message_size;
        MPI_Get_count(&status, MPI_CHAR, &message_size);

        char *message = (char *)malloc((message_size + 1) * sizeof(char));
        if (!message)
        {
            fprintf(log_file, "Tracker: Memory allocation failed\n");
            fflush(log_file);
            MPI_Abort(MPI_COMM_WORLD, -1);
        }

        MPI_Recv(message, message_size, MPI_CHAR,
                 status.MPI_SOURCE, status.MPI_TAG,
                 MPI_COMM_WORLD, &status);
        message[message_size] = '\0';

        int sender_rank = status.MPI_SOURCE;
        fprintf(log_file, "Tracker: Received message of size %d from rank %d with tag %d\n",
                message_size, sender_rank, status.MPI_TAG);
        fflush(log_file);

        if (status.MPI_TAG == MSG_LIST_PEERS)
        {
            char requested_filename[MAX_FILENAME];
            sscanf(message, "LIST_PEERS %s", requested_filename);

            int file_index = -1;
            for (int i = 0; i < tracker_file_count; i++)
            {
                if (strcmp(tracker_files[i].filename, requested_filename) == 0)
                {
                    file_index = i;
                    break;
                }
            }

            if (file_index != -1)
            {
                // Trimite lista de peers
                char peer_list[1024] = "";
                sprintf(peer_list, "%d", tracker_files[file_index].total_segments);
                for (int j = 0; j < MAX_PEERS; j++)
                {
                    if (tracker_files[file_index].clients_with_file[j])
                    {
                        char rank_str[16];
                        sprintf(rank_str, " %d", j);
                        strcat(peer_list, rank_str);
                    }
                }
                MPI_Send(peer_list, strlen(peer_list) + 1, MPI_CHAR, sender_rank, MSG_PEER_LIST, MPI_COMM_WORLD);

                // Trimite hash-urile segmentelor separat
                for (int s = 0; s < tracker_files[file_index].total_segments; s++)
                {
                    char hash_message[256];
                    sprintf(hash_message, "HASH %d %s", s, tracker_files[file_index].segment_hashes[s]);
                    MPI_Send(hash_message, strlen(hash_message) + 1, MPI_CHAR, sender_rank, MSG_PEER_LIST, MPI_COMM_WORLD);
                }
            }
        }
        else if (status.MPI_TAG == MSG_RECEIVED_SEGMENT)
        {
            char filename[MAX_FILENAME];
            sscanf(message, "RECEIVED_SEGMENT %s", filename);

            fprintf(log_file, "Tracker: Peer %d received a segment of %s.\n", sender_rank, filename);
            fflush(log_file);

            for (int i = 0; i < tracker_file_count; i++)
            {
                if (strcmp(tracker_files[i].filename, filename) == 0)
                {
                    tracker_files[i].clients_with_file[sender_rank] = 1; // Marcheaza peer-ul ca avand partial fisierul
                    break;
                }
            }
        }
        else if (status.MPI_TAG == MSG_FINISH_DOWNLOAD)
        {
            char filename[MAX_FILENAME];
            sscanf(message, "FINISH_DOWNLOAD %s", filename);
            fprintf(log_file, "Tracker: Peer %d has finished downloading %s.\n", sender_rank, filename);
            fflush(log_file);

            for (int i = 0; i < tracker_file_count; i++)
            {
                if (strcmp(tracker_files[i].filename, filename) == 0)
                {
                    tracker_files[i].clients_with_file[sender_rank] = 1; // Marcheaza peer-ul ca avand fisierul full
                    break;
                }
            }
        }
        else if (status.MPI_TAG == MSG_FINALIZE_ALL)
        {
            fprintf(log_file, "Tracker: Peer %d has finalized all downloads.\n", sender_rank);
            fflush(log_file);
            clients_finalized++;
        }

        free(message);
    }

    for (int i = 1; i < numtasks; i++)
    {
        MPI_Send("TERMINATE", strlen("TERMINATE") + 1, MPI_CHAR, i,
                 MSG_DOWNLOAD_REQUEST, MPI_COMM_WORLD); // trimis catre thread ul de upload
        fprintf(log_file, "Tracker: Sent TERMINATE to Peer %d.\n", i);
        fflush(log_file);
    }
}

// Functia de citire a fisierului de input
void read_input_file(int rank, PeerInfo *peer_info)
{
    char input_filename[20], output_filename[20];
    sprintf(input_filename, "in%d.txt", rank);
    sprintf(output_filename, "o%d.txt", rank);

    FILE *input_file = fopen(input_filename, "r");
    if (!input_file)
    {
        fprintf(log_file, "Peer %d: Error opening input file %s\n", rank, input_filename);
        fflush(log_file);
        MPI_Abort(MPI_COMM_WORLD, -1);
    }

    FILE *output_file = fopen(output_filename, "a");
    if (!output_file)
    {
        fprintf(log_file, "Peer %d: Error opening output file %s\n", rank, output_filename);
        fflush(log_file);
        fclose(input_file);
        MPI_Abort(MPI_COMM_WORLD, -1);
    }

    fscanf(input_file, "%d", &peer_info->owned_file_count);
    fprintf(output_file, "%d\n", peer_info->owned_file_count);

    for (int i = 0; i < peer_info->owned_file_count; i++)
    {
        fscanf(input_file, "%s %d",
               peer_info->owned_files[i].filename,
               &peer_info->owned_files[i].total_segments);
        fprintf(output_file, "%s %d\n",
                peer_info->owned_files[i].filename,
                peer_info->owned_files[i].total_segments);

        for (int j = 0; j < peer_info->owned_files[i].total_segments; j++)
        {
            fscanf(input_file, "%s", peer_info->owned_files[i].segments[j]);
            peer_info->owned_files[i].segments[j][HASH_SIZE] = '\0';
            fprintf(output_file, "%s\n", peer_info->owned_files[i].segments[j]);
        }
        fprintf(output_file, "\n");
    }

    fscanf(input_file, "%d", &peer_info->requested_file_count);
    fprintf(output_file, "%d\n", peer_info->requested_file_count);

    for (int i = 0; i < peer_info->requested_file_count; i++)
    {
        fscanf(input_file, "%s", peer_info->requested_files[i]);
        fprintf(output_file, "%s\n", peer_info->requested_files[i]);
    }

    fclose(input_file);
    fclose(output_file);
}

// Trimite info despre fisiere la tracker
void send_file_info_to_tracker(int rank, PeerInfo *peer_info)
{
    // Construim mesajul INIT
    char init_message[1024];
    memset(init_message, 0, sizeof(init_message));

    sprintf(init_message, "INIT %d", peer_info->owned_file_count);

    // Adauga numele fisierelor si numarul de segmente
    for (int i = 0; i < peer_info->owned_file_count; i++)
    {
        char tmp[128];
        sprintf(tmp, " %s %d",
                peer_info->owned_files[i].filename,
                peer_info->owned_files[i].total_segments);
        strcat(init_message, tmp);
    }

    // Trimite INIT
    MPI_Send(init_message, strlen(init_message) + 1, MPI_CHAR,
             TRACKER_RANK, MSG_INIT, MPI_COMM_WORLD);

    char ack_buffer[16] = {0};
    MPI_Status ack_status;
    fflush(stdout);

    // Trimite segmentele (UPLOAD)
    for (int i = 0; i < peer_info->owned_file_count; i++)
    {
        for (int j = 0; j < peer_info->owned_files[i].total_segments; j++)
        {
            char upload_message[256];
            sprintf(upload_message, "UPLOAD %s %d %s",
                    peer_info->owned_files[i].filename,     
                    j,                                      
                    peer_info->owned_files[i].segments[j]);

            MPI_Send(upload_message, strlen(upload_message) + 1, MPI_CHAR,
                     TRACKER_RANK, MSG_UPLOAD, MPI_COMM_WORLD);

            fprintf(stdout, "Peer %d: Sent UPLOAD for file %s, segment %d, hash %s.\n",
                    rank, peer_info->owned_files[i].filename, j, peer_info->owned_files[i].segments[j]);
            fflush(stdout);
        }
    }

    // Asteapta  ACK pentru finalizarea UPLOAD-urilor
    MPI_Recv(ack_buffer, 16, MPI_CHAR, TRACKER_RANK, MSG_ACK, MPI_COMM_WORLD, &ack_status);
    fprintf(stdout, "Peer %d: Received ACK from tracker for UPLOAD completion.\n", rank);
    fflush(stdout);
}

// Salveaza fisierul descarcat in fisierul specificat de cerinta
void save_downloaded_file(int rank, const char *filename, PeerInfo *peer_info)
{
    char output_filename[100];
    sprintf(output_filename, "client%d_%s", rank, filename);
    FILE *output_file = fopen(output_filename, "w");
    if (!output_file)
    {
        fprintf(log_file, "Peer %d: Error creating output file %s\n", rank, output_filename);
        fflush(log_file);
        return;
    }

    for (int i = 0; i < peer_info->owned_file_count; i++)
    {
        if (strcmp(peer_info->owned_files[i].filename, filename) == 0)
        {
            for (int j = 0; j < peer_info->owned_files[i].total_segments; j++)
            {
                if (strlen(peer_info->owned_files[i].segments[j]) > 0)
                {
                    fprintf(output_file, "%s\n", peer_info->owned_files[i].segments[j]);
                }
                else
                {
                    fprintf(output_file, "MISSING_SEGMENT_%d\n", j); // Diagnostic
                }
            }
            break;
        }
    }

    fclose(output_file);
    fprintf(log_file, "Peer %d: Saved downloaded file %s.\n", rank, output_filename);
    fflush(log_file);
}

// Firul de upload - raspunde cererilor de segmente
void *upload_thread_func(void *arg)
{
    ThreadArgs *thread_args = (ThreadArgs *)arg;
    int rank = thread_args->rank;
    PeerInfo *peer_info = thread_args->peer_info;
    pthread_mutex_t *mutex = thread_args->peer_info_mutex;
    MPI_Status status;
    char message[256];

    while (1)
    {
        MPI_Recv(message, 256, MPI_CHAR, MPI_ANY_SOURCE,
                 MSG_DOWNLOAD_REQUEST, MPI_COMM_WORLD, &status);

        if (strcmp(message, "TERMINATE") == 0)
        {
            fprintf(log_file, "Peer %d: Received TERMINATE signal. Exiting upload thread.\n", rank);
            fflush(log_file);
            break;
        }

        char requested_filename[MAX_FILENAME];
        int segment_index;
        sscanf(message, "%s %d", requested_filename, &segment_index);

        int file_index = -1;
        int has_segment = 0;

        pthread_mutex_lock(mutex);
        for (int i = 0; i < peer_info->owned_file_count; i++)
        {
            if (strcmp(peer_info->owned_files[i].filename, requested_filename) == 0)
            {
                if (segment_index < peer_info->owned_files[i].total_segments)
                {
                    has_segment = 1;
                    file_index = i;
                    break;
                }
            }
        }
        pthread_mutex_unlock(mutex);

        if (has_segment)
        {
            const char *hash_value = peer_info->owned_files[file_index].segments[segment_index];
            char response[256];
            sprintf(response, "HASH %s", hash_value);
            MPI_Send(response, strlen(response) + 1, MPI_CHAR,
                     status.MPI_SOURCE, MSG_DOWNLOAD_RESPONSE, MPI_COMM_WORLD);
            fprintf(log_file, "Peer %d: Sent hash for segment %d to peer %d.\n",
                    rank, segment_index, status.MPI_SOURCE);
            fflush(log_file);
        }
        else
        {
            MPI_Send("NACK", 5, MPI_CHAR, status.MPI_SOURCE,
                     MSG_DOWNLOAD_RESPONSE, MPI_COMM_WORLD);
            fprintf(log_file, "Peer %d: NACK for segment %d, file %s.\n",
                    rank, segment_index, requested_filename);
            fflush(log_file);
        }
    }

    return NULL;
}

// Salveaza un segment nou primit pentru un fisier
void store_segment_locally(const char *filename, int segment_index, const char *hash_value)
{
    int found_file_index = -1;
    for (int fi = 0; fi < global_peer_info.owned_file_count; fi++)
    {
        if (strcmp(global_peer_info.owned_files[fi].filename, filename) == 0)
        {
            found_file_index = fi;
            break;
        }
    }
    if (found_file_index == -1)
    {
        found_file_index = global_peer_info.owned_file_count++;
        strcpy(global_peer_info.owned_files[found_file_index].filename, filename);
        global_peer_info.owned_files[found_file_index].total_segments = 0;
    }
    strcpy(global_peer_info.owned_files[found_file_index].segments[segment_index],
           hash_value);

    if (segment_index + 1 > global_peer_info.owned_files[found_file_index].total_segments)
    {
        global_peer_info.owned_files[found_file_index].total_segments = segment_index + 1;
    }
}

// Firul de download
void *download_thread_func(void *arg)
{
    ThreadArgs *thread_args = (ThreadArgs *)arg;
    int rank = thread_args->rank;
    PeerInfo *peer_info = thread_args->peer_info;
    pthread_mutex_t *mutex = thread_args->peer_info_mutex;
    MPI_Status status;
    char message[1024];

    DownloadInfo downloads[MAX_FILES];
    memset(downloads, 0, sizeof(downloads));

    for (int i = 0; i < peer_info->requested_file_count; i++)
    {
        strcpy(downloads[i].filename, peer_info->requested_files[i]);
        downloads[i].segments_downloaded = 0;
        downloads[i].segments_total = MAX_CHUNKS;
        downloads[i].peer_count = 0;
    }

    for (int i = 0; i < peer_info->requested_file_count; i++)
    {
        char request_message[256];
        sprintf(request_message, "LIST_PEERS %s", peer_info->requested_files[i]);
        MPI_Send(request_message, strlen(request_message) + 1, MPI_CHAR, TRACKER_RANK, MSG_LIST_PEERS, MPI_COMM_WORLD);

        char peer_list[1024];
        MPI_Recv(peer_list, sizeof(peer_list), MPI_CHAR, TRACKER_RANK, MSG_PEER_LIST, MPI_COMM_WORLD, &status);

        int seg_count = 0;
        sscanf(peer_list, "%d", &seg_count);
        downloads[i].segments_total = seg_count;

        char *ptr = strtok(peer_list, " ");
        int peer_index = 0;
        while ((ptr = strtok(NULL, " ")) != NULL)
        {
            downloads[i].peers[peer_index++] = atoi(ptr);
        }
        downloads[i].peer_count = peer_index;

        // Primim hash-urile segmentelor
        for (int s = 0; s < downloads[i].segments_total; s++)
        {
            char hash_message[256];
            MPI_Recv(hash_message, sizeof(hash_message), MPI_CHAR, TRACKER_RANK, MSG_PEER_LIST, MPI_COMM_WORLD, &status);

            int segment_index;
            char hash_value[HASH_SIZE + 1];
            sscanf(hash_message, "HASH %d %s", &segment_index, hash_value);
            strcpy(downloads[i].filename_hashes[s], hash_value);
        }
    }

    // Descarcam
    for (int i = 0; i < peer_info->requested_file_count; i++)
    {
        for (int segment = 0; segment < downloads[i].segments_total; segment++)
        {
            int successful_download = 0;
            int attempts = 0;
            while (!successful_download && attempts < downloads[i].peer_count)
            {
                int peer_to_request = downloads[i].peers[(segment + attempts) % downloads[i].peer_count]; // Alegere circulara a peer ului de la care descarcam

                attempts++;

                if (peer_to_request == rank)
                    continue;

                char request_segment[256];
                sprintf(request_segment, "%s %d", downloads[i].filename, segment);
                MPI_Send(request_segment, strlen(request_segment) + 1, MPI_CHAR,
                         peer_to_request, MSG_DOWNLOAD_REQUEST, MPI_COMM_WORLD);

                fprintf(log_file, "Peer %d: Requested %s segment %d from Peer %d.\n",
                        rank, downloads[i].filename, segment, peer_to_request);
                fflush(log_file);

                MPI_Recv(message, 256, MPI_CHAR, peer_to_request, MSG_DOWNLOAD_RESPONSE, MPI_COMM_WORLD, &status);

                if (strncmp(message, "NACK", 4) == 0)
                {
                    fprintf(log_file, "Peer %d: NACK for segment %d, file %s from peer %d.\n",
                            rank, segment, downloads[i].filename, peer_to_request);
                    fflush(log_file);
                }
                else if (strncmp(message, "HASH", 4) == 0)
                {
                    char dummy[8];
                    char hash_value[HASH_SIZE + 1];
                    sscanf(message, "%s %s", dummy, hash_value);

                    if (strcmp(hash_value, downloads[i].filename_hashes[segment]) == 0)
                    {
                        pthread_mutex_lock(mutex);
                        store_segment_locally(downloads[i].filename, segment, hash_value);
                        pthread_mutex_unlock(mutex);

                        downloads[i].segments_downloaded++;
                        successful_download = 1;

                        fprintf(log_file, "Peer %d: Successfully downloaded segment %d of %s from Peer %d: %s\n",
                                rank, segment, downloads[i].filename, peer_to_request, hash_value);
                        fflush(log_file);
                    } else {
                        fprintf(log_file, "Peer %d: Failed to download segment %d of %s from Peer %d: %s\n %s\n",
                                rank, segment, downloads[i].filename, peer_to_request, hash_value , downloads[i].filename_hashes[segment]);
                        fflush(log_file);
                    }

                    if (downloads[i].segments_downloaded == 1) // Dupa primul segment descarcat
                    {
                        char notify_tracker[256];
                        sprintf(notify_tracker, "RECEIVED_SEGMENT %s", downloads[i].filename);
                        MPI_Send(notify_tracker, strlen(notify_tracker) + 1, MPI_CHAR,
                                 TRACKER_RANK, MSG_RECEIVED_SEGMENT, MPI_COMM_WORLD);
                        fprintf(log_file, "Peer %d: Notified tracker about partial ownership of %s.\n",
                                rank, downloads[i].filename);
                        fflush(log_file);
                    }
                }
            }

            if (!successful_download)
            {
                fprintf(log_file, "Peer %d: Could not download segment %d of file %s from any peer.\n",
                        rank, segment, downloads[i].filename);
                fflush(log_file);
            }

            // re-actualizez la fiecare 10 segmente
            if (downloads[i].segments_downloaded > 0 &&
                downloads[i].segments_downloaded % SEGMENT_REQUEST_BATCH == 0)
            {
                char request_message[256];
                sprintf(request_message, "LIST_PEERS %s", downloads[i].filename);
                MPI_Send(request_message, strlen(request_message) + 1, MPI_CHAR,
                         TRACKER_RANK, MSG_LIST_PEERS, MPI_COMM_WORLD);
                fprintf(log_file, "Peer %d: Re-requested peer list for file %s after %d segments.\n",
                        rank, downloads[i].filename, downloads[i].segments_downloaded);
                fflush(log_file);

                MPI_Recv(message, 1024, MPI_CHAR, TRACKER_RANK,
                         MSG_PEER_LIST, MPI_COMM_WORLD, &status);

                int seg_count2;
                char *token = strtok(message, " ");
                if (token != NULL)
                    seg_count2 = atoi(token);

                int pc2 = 0;
                while ((token = strtok(NULL, " ")) != NULL)
                {
                    int p2 = atoi(token);
                    downloads[i].peers[pc2++] = p2;
                }
                downloads[i].peer_count = pc2;

                fprintf(log_file, "Peer %d: Updated peers for file %s: ",
                        rank, downloads[i].filename);
                for (int p = 0; p < pc2; p++)
                {
                    fprintf(log_file, "%d ", downloads[i].peers[p]);
                }
                fprintf(log_file, "\n");
                fflush(log_file);

                for (int s = 0; s < seg_count2; s++)
                {
                    MPI_Recv(message, 256, MPI_CHAR, TRACKER_RANK, MSG_PEER_LIST, MPI_COMM_WORLD, &status);
                }
            }
        }
            char finalize_download[256];
            sprintf(finalize_download, "FINISH_DOWNLOAD %s", downloads[i].filename);
            MPI_Send(finalize_download, strlen(finalize_download) + 1, MPI_CHAR,
                     TRACKER_RANK, MSG_FINISH_DOWNLOAD, MPI_COMM_WORLD);
            fprintf(log_file, "Peer %d: Sent FINISH_DOWNLOAD for file %s.\n",
                    rank, downloads[i].filename);
            fflush(log_file);

            save_downloaded_file(rank, downloads[i].filename, peer_info);
        
        }
        char finalize_all[256];
        sprintf(finalize_all, "FINALIZE_ALL %d", rank);
        MPI_Send(finalize_all, strlen(finalize_all) + 1, MPI_CHAR,
                 TRACKER_RANK, MSG_FINALIZE_ALL, MPI_COMM_WORLD);
        fprintf(log_file, "Peer %d: Sent FINALIZE_ALL.\n", rank);
        fflush(log_file);

        pthread_exit(NULL);
        return NULL;
    
}
    // Functia peer
    void peer(int numtasks, int rank)
    {
        pthread_t download_thread;
        pthread_t upload_thread;
        void *status;

        read_input_file(rank, &global_peer_info);

        send_file_info_to_tracker(rank, &global_peer_info);

        pthread_mutex_t peer_info_mutex = PTHREAD_MUTEX_INITIALIZER;

        ThreadArgs thread_args;
        thread_args.rank = rank;
        thread_args.peer_info = &global_peer_info;
        thread_args.peer_info_mutex = &peer_info_mutex;

        if (pthread_create(&download_thread, NULL, download_thread_func, (void *)&thread_args))
        {
            fprintf(log_file, "Peer %d: Error creating download thread.\n", rank);
            fflush(log_file);
            exit(-1);
        }

        if (pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&thread_args))
        {
            fprintf(log_file, "Peer %d: Error creating upload thread.\n", rank);
            fflush(log_file);
            exit(-1);
        }

        if (pthread_join(download_thread, &status))
        {
            fprintf(log_file, "Peer %d: Error joining download thread.\n", rank);
            fflush(log_file);
            exit(-1);
        }

        if (pthread_join(upload_thread, &status))
        {
            fprintf(log_file, "Peer %d: Error joining upload thread.\n", rank);
            fflush(log_file);
            exit(-1);
        }

        pthread_mutex_destroy(&peer_info_mutex);
    }

    int main(int argc, char *argv[])
    {
        int numtasks, rank;
        int provided;

        MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
        if (provided < MPI_THREAD_MULTIPLE)
        {
            fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
            exit(-1);
        }

        MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);

        char log_filename[32];
        sprintf(log_filename, "o%d.txt", rank);
        log_file = fopen(log_filename, "w");
        if (!log_file)
        {
            fprintf(stderr, "Cannot open %s for writing logs.\n", log_filename);
            MPI_Abort(MPI_COMM_WORLD, -1);
        }

        if (rank == TRACKER_RANK)
        {
            tracker(numtasks, rank);
        }
        else
        {
            peer(numtasks, rank);
        }

        fclose(log_file);
        MPI_Finalize();
        return 0;
    }