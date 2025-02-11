MinPlay is a minimalistic media player which supports virtually all media formats. Based on FFmpeg(media handling), Qt(GUI) and SDL3(video and audio output).
![image](https://github.com/user-attachments/assets/e000533d-7a0c-4b62-b68f-80600883d785)

The player implements only basic functionality(simple playlist, seeking, pause/resume, stream switching etc) and utilizes multithreading to ensure better playback experience.

Design:
The playback engine spawns a number of threads that are being controlled from the main(GUI) thread. The threads are:
1) Demuxer thread - reads a stream from a file or network. Responsible for controlling decoder threads and handling requests from the main thread(opening streams, seeking, stream switching etc).
2) Decoder threads(video/audio/subtitles) - decode packets from the demuxer and fill data queues with them.
3) The main(GUI) thread - pulls data from the decoder queues as necessary and pushes the data to video/audio outputs. Also controls the demuxer thread.
