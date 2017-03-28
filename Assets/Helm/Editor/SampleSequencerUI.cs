// Copyright 2017 Matt Tytel

using UnityEditor;
using UnityEngine;

namespace Helm
{
    [CustomEditor(typeof(SampleSequencer))]
    class SampleSequencerUI : Editor
    {
        const float keyboardWidth = 30.0f;
        const float scrollWidth = 15.0f;

        private SerializedObject serialized;
        SequencerUI sequencer = new SequencerUI(keyboardWidth, scrollWidth + 1);
        SequencerPositionUI sequencerPosition = new SequencerPositionUI(keyboardWidth, scrollWidth);
        SequencerVelocityUI velocities = new SequencerVelocityUI(keyboardWidth, scrollWidth);
        SerializedProperty length;

        float positionHeight = 10.0f;
        float velocitiesHeight = 40.0f;
        float sequencerHeight = 400.0f;
        const float minWidth = 200.0f;

        void OnEnable()
        {
            length = serializedObject.FindProperty("length");
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();

            Color prev_color = GUI.backgroundColor;
            GUILayout.Space(5f);
            SampleSequencer sampleSequencer = target as SampleSequencer;
            Rect sequencerPositionRect = GUILayoutUtility.GetRect(minWidth, positionHeight, GUILayout.ExpandWidth(true));
            Rect rect = GUILayoutUtility.GetRect(minWidth, sequencerHeight, GUILayout.ExpandWidth(true));
            Rect velocitiesRect = GUILayoutUtility.GetRect(minWidth, velocitiesHeight, GUILayout.ExpandWidth(true));

            if (sequencer.DoSequencerEvents(rect, sampleSequencer))
                Repaint();
            if (velocities.DoVelocityEvents(velocitiesRect, sampleSequencer))
                Repaint();

            sequencerPosition.DrawSequencerPosition(sequencerPositionRect, sampleSequencer);
            velocities.DrawSequencerPosition(velocitiesRect, sampleSequencer);

            if (rect.height == sequencerHeight)
                sequencer.DrawSequencer(rect, sampleSequencer);
            GUILayout.Space(5f);
            GUI.backgroundColor = prev_color;

            if (GUILayout.Button("Clear Sequencer"))
            {
                Undo.RecordObject(sampleSequencer, "Clear Sequencer");
                sampleSequencer.Clear();
            }

            EditorGUILayout.IntSlider(length, 1, Sequencer.kMaxLength);
            serializedObject.ApplyModifiedProperties();
        }
    }
}
