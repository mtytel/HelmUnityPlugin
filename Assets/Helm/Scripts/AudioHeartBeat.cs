// Copyright 2017 Matt Tytel

using UnityEngine;

namespace Tytel
{
    [ExecuteInEditMode]
    [RequireComponent(typeof(AudioSource))]
    public class AudioHeartBeat : MonoBehaviour
    {
        void Awake()
        {
            Utils.InitAudioSource(GetComponent<AudioSource>());
        }
    }
}
