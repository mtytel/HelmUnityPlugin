// Copyright 2017 Matt Tytel

using UnityEngine;
using System.Runtime.InteropServices;

namespace Helm
{
    public class HelmBpm : MonoBehaviour
    {
        [DllImport("AudioPluginHelm")]
        private static extern void SetBpm(float bpm);

        private static float globalBpm = 120.0f;

        [SerializeField]
        private float bpm_ = 120.0f;
        public float bpm
        {
            get
            {
                return bpm_;
            }
            set
            {
                bpm_ = value;
                SetGlobalBpm();
            }
        }

        void OnEnable()
        {
            SetGlobalBpm();
        }

        public void SetGlobalBpm()
        {
            if (bpm_ > 0.0f)
            {
                SetBpm(bpm_);
                globalBpm = bpm_;
            }
        }

        public static float GetGlobalBpm()
        {
            return globalBpm;
        }
    }
}
