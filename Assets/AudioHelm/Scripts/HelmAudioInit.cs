// Copyright 2017 Matt Tytel

using UnityEngine;
using UnityEngine.Audio;

namespace AudioHelm
{
    /// <summary>
    /// ## [Switch to Manual](../manual/class_audio_helm_1_1_helm_audio_init.html)<br>
    /// Ensures AudioSource and global AudioSettings are setup correctly for Helm native synthesizer usage.
    /// </summary>
    [ExecuteInEditMode]
    [RequireComponent(typeof(AudioSource))]
    [AddComponentMenu("Audio Helm/Helm Audio Init")]
    [HelpURL("http://tytel.org/audiohelm/manual/class_audio_helm_1_1_helm_audio_init.html")]
    public class HelmAudioInit : MonoBehaviour
    {
        bool warnedNoAudioGroup = false;

        [Tooltip("The Audio Mixer Group where the Helm synthesizer instance is running.")]
        public AudioMixerGroup synthesizerMixerGroup;

        [Tooltip("The Audio Mixer Group where the spatialized audio should route to.")]
        public AudioMixerGroup spatializerMixerGroup;

        AudioSource sendAudioSource = null;
        HelmAudioReceive receiveAudio = null;

        int GetChannel()
        {
            HelmController controller = GetComponent<HelmController>();
            if (controller)
                return controller.channel;

            HelmSequencer sequencer = GetComponent<HelmSequencer>();
            if (sequencer)
                return sequencer.channel;

            return 0;
        }

        void SetupSpatialization(AudioSource audioComponent)
        {
            if (synthesizerMixerGroup == null || spatializerMixerGroup == null)
            {
                Debug.LogWarning("If spatialization is enabled on the Audio Source you must set the " +
                                 "synthesizer and spatializer mixer groups on the HelmAudioInit component!");
                return;
            }

            audioComponent.spatializePostEffects = true;
            audioComponent.outputAudioMixerGroup = spatializerMixerGroup;
            receiveAudio = gameObject.AddComponent<HelmAudioReceive>();
            receiveAudio.channel = GetChannel();

            GameObject sendAudioObject = new GameObject("__HelmSendSignal__");
            sendAudioObject.transform.parent = transform;
            sendAudioObject.transform.localPosition = Vector3.zero;
            sendAudioObject.transform.localRotation = Quaternion.identity;

            sendAudioSource = sendAudioObject.AddComponent<AudioSource>();
            Utils.InitAudioSource(sendAudioSource);
            sendAudioSource.outputAudioMixerGroup = synthesizerMixerGroup;
        }

        void Awake()
        {
            Utils.InitAudioSource(GetComponent<AudioSource>());

            if (Application.isPlaying)
            {
                AudioSource audioComponent = GetComponent<AudioSource>();
                if (audioComponent.spatialize)
                    SetupSpatialization(audioComponent);
            }
        }

        void Update()
        {
            AudioSource audioComponent = GetComponent<AudioSource>();
            Native.HelmSilence(GetChannel(), audioComponent.spatialize);

            // Make sure AudioSource is setup correctly.
            if (Application.isPlaying && audioComponent.outputAudioMixerGroup == null)
            {
                if (!warnedNoAudioGroup)
                {
                    Debug.LogWarning("AudioSource output needs an AudioMixerGroup with a Helm Instance.");
                    warnedNoAudioGroup = true;
                }
            }
            else
                warnedNoAudioGroup = false;

            audioComponent.pitch = 1.0f;
            audioComponent.dopplerLevel = 0.0f;
            if (sendAudioSource)
            {
                sendAudioSource.priority = audioComponent.priority;
                sendAudioSource.volume = audioComponent.volume;
                sendAudioSource.panStereo = audioComponent.panStereo;
                sendAudioSource.spatialBlend = 0.0f;
            }
        }
    }
}
