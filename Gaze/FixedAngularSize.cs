﻿using UnityEngine;

namespace HoloToolkit
{
    public class FixedAngularSize : MonoBehaviour
    {
        Vector3 defaultSizeRatios;

        void Start()
        {
            // Calculate the XYZ ratios for the transform's localScale over its
            // initial distance from the camera.
            float startingDistance = Vector3.Distance(Camera.main.transform.position, transform.position);
            if (startingDistance > 0.0f)
            {
                defaultSizeRatios = transform.localScale / startingDistance;
            }
            else
            {
                // If the transform and the camera are both in the same position
                // (that is, the distance between them is zero), all bets are off.
                // Disable this Behaviour.
                enabled = false;
#if UNITY_EDITOR
                Debug.Log("The object and the camera are in the same position at Start(). The attached FixedAngularSize Behaviour is now disabled.");
#endif // UNITY_EDITOR
            }
        }

        void Update()
        {
            float distanceToHologram = Vector3.Distance(Camera.main.transform.position, transform.position);
            transform.localScale = defaultSizeRatios * distanceToHologram;
        }
    }
}